resource "aws_ecr_repository" "this" {
  name = "${var.name_prefix}-backend"

  # Immutable tags are what make rollback possible: deploys push the git SHA,
  # and a SHA that already exists cannot be silently overwritten with different
  # content.
  image_tag_mutability = "IMMUTABLE"

  image_scanning_configuration {
    scan_on_push = true
  }

  # terraform destroy must not leave billable images behind.
  force_delete = true
}

resource "aws_cloudwatch_log_group" "this" {
  name              = "/ecs/${var.name_prefix}-backend"
  retention_in_days = 14
}

data "aws_iam_policy_document" "assume" {
  statement {
    actions = ["sts:AssumeRole"]
    principals {
      type        = "Service"
      identifiers = ["ecs-tasks.amazonaws.com"]
    }
  }
}

# ECR pull, CloudWatch Logs writes and Secrets Manager reads are all EXECUTION
# role permissions -- the agent uses them to start the container. The spec calls
# them task-role permissions; that is the one place it is technically wrong.
resource "aws_iam_role" "execution" {
  name               = "${var.name_prefix}-ecs-execution"
  assume_role_policy = data.aws_iam_policy_document.assume.json
}

resource "aws_iam_role_policy_attachment" "execution" {
  role       = aws_iam_role.execution.name
  policy_arn = "arn:aws:iam::aws:policy/service-role/AmazonECSTaskExecutionRolePolicy"
}

data "aws_iam_policy_document" "secrets" {
  statement {
    actions   = ["secretsmanager:GetSecretValue"]
    resources = [var.database_url_secret_arn, var.redis_url_secret_arn]
  }
}

resource "aws_iam_role_policy" "secrets" {
  name   = "${var.name_prefix}-read-secrets"
  role   = aws_iam_role.execution.id
  policy = data.aws_iam_policy_document.secrets.json
}

# The task role is what the application itself assumes. Nothing in backend/src
# calls an AWS API, so it gets no policy at all -- least privilege by having
# nothing to grant, rather than by trimming.
resource "aws_iam_role" "task" {
  name               = "${var.name_prefix}-ecs-task"
  assume_role_policy = data.aws_iam_policy_document.assume.json
}

resource "aws_lb" "this" {
  name               = "${var.name_prefix}-alb"
  load_balancer_type = "application"
  internal           = false
  subnets            = var.public_subnet_ids
  security_groups    = [var.alb_security_group_id]
}

resource "aws_lb_target_group" "this" {
  name        = "${var.name_prefix}-backend"
  port        = 3000
  protocol    = "HTTP"
  vpc_id      = var.vpc_id
  target_type = "ip"

  # /v1/health/live, NOT /v1/health. /v1/health returns 200 unconditionally
  # with status hardcoded to 'ok', so it would report a task with a dead
  # database as healthy. And with one task and one shared RDS instance,
  # failing the check on a database outage would deregister the only target
  # and turn the backend's retryable 503s into CloudFront 502s without routing
  # around anything.
  health_check {
    path                = "/v1/health/live"
    matcher             = "200"
    interval            = 30
    timeout             = 5
    healthy_threshold   = 2
    unhealthy_threshold = 3
  }

  # Long enough for an in-flight ingest to finish once the task is draining,
  # and comfortably inside the task's own stopTimeout.
  deregistration_delay = 30
}

resource "aws_lb_listener" "http" {
  load_balancer_arn = aws_lb.this.arn
  port              = 80
  protocol          = "HTTP"

  default_action {
    type             = "forward"
    target_group_arn = aws_lb_target_group.this.arn
  }
}

resource "aws_ecs_cluster" "this" {
  name = "${var.name_prefix}-cluster"
}

resource "aws_ecs_task_definition" "this" {
  family                   = "${var.name_prefix}-backend"
  requires_compatibilities = ["FARGATE"]
  network_mode             = "awsvpc"
  cpu                      = var.cpu
  memory                   = var.memory
  execution_role_arn       = aws_iam_role.execution.arn
  task_role_arn            = aws_iam_role.task.arn

  container_definitions = jsonencode([
    {
      name  = "backend"
      image = "${aws_ecr_repository.this.repository_url}:bootstrap"

      # SIGTERM, then SIGKILL after this many seconds. Task 1's shutdown hooks
      # need room to close the pg pool and the Redis client; 60 is well beyond
      # any observed insert and still inside Fargate's 120s ceiling.
      stopTimeout = 60

      portMappings = [{ containerPort = 3000, protocol = "tcp" }]

      environment = [
        { name = "PORT", value = "3000" },
        { name = "IDEMPOTENCY_TTL_SECONDS", value = "900" },
      ]

      secrets = [
        { name = "DATABASE_URL", valueFrom = var.database_url_secret_arn },
        { name = "REDIS_URL", valueFrom = var.redis_url_secret_arn },
      ]

      logConfiguration = {
        logDriver = "awslogs"
        options = {
          "awslogs-group"         = aws_cloudwatch_log_group.this.name
          "awslogs-region"        = data.aws_region.current.name
          "awslogs-stream-prefix" = "backend"
        }
      }
    }
  ])

  # Deploys register a new revision pointing at an immutable git-SHA tag, so
  # Terraform must not fight the deploy script over the image. terraform apply
  # provisions infrastructure; scripts/deploy-backend.sh deploys code.
  lifecycle {
    ignore_changes = [container_definitions]
  }
}

data "aws_region" "current" {}

resource "aws_ecs_service" "this" {
  name            = "${var.name_prefix}-backend"
  cluster         = aws_ecs_cluster.this.id
  task_definition = aws_ecs_task_definition.this.arn
  desired_count   = 1
  launch_type     = "FARGATE"

  network_configuration {
    subnets         = var.public_subnet_ids
    security_groups = [var.ecs_security_group_id]
    # No NAT Gateway, so the task needs a public IP to pull from ECR and write
    # to CloudWatch Logs. Inbound is still ALB-only, via the security group.
    assign_public_ip = true
  }

  load_balancer {
    target_group_arn = aws_lb_target_group.this.arn
    container_name   = "backend"
    container_port   = 3000
  }

  # The service is created before any image exists. The first tasks fail to
  # pull and ECS retries with backoff until the first deploy pushes one; that
  # is expected on a first apply, not an error to work around.
  lifecycle {
    ignore_changes = [task_definition]
  }

  depends_on = [aws_lb_listener.http]
}
