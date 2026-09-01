module "network" {
  source = "./modules/network"

  name_prefix = var.name_prefix
  vpc_cidr    = var.vpc_cidr
}

module "rds" {
  source = "./modules/rds"

  name_prefix       = var.name_prefix
  subnet_ids        = module.network.private_subnet_ids
  security_group_id = module.network.rds_security_group_id
  instance_class    = var.db_instance_class
}

module "elasticache" {
  source = "./modules/elasticache"

  name_prefix       = var.name_prefix
  subnet_ids        = module.network.private_subnet_ids
  security_group_id = module.network.redis_security_group_id
  node_type         = var.redis_node_type
}

# The task definition references these by ARN rather than carrying the values,
# so the connection strings never appear in the ECS console or in a task
# definition revision.
resource "aws_secretsmanager_secret" "database_url" {
  name                    = "${var.name_prefix}/DATABASE_URL"
  recovery_window_in_days = 0
}

resource "aws_secretsmanager_secret_version" "database_url" {
  secret_id     = aws_secretsmanager_secret.database_url.id
  secret_string = module.rds.connection_url
}

resource "aws_secretsmanager_secret" "redis_url" {
  name                    = "${var.name_prefix}/REDIS_URL"
  recovery_window_in_days = 0
}

resource "aws_secretsmanager_secret_version" "redis_url" {
  secret_id     = aws_secretsmanager_secret.redis_url.id
  secret_string = module.elasticache.connection_url
}

module "ecs" {
  source = "./modules/ecs"

  name_prefix             = var.name_prefix
  vpc_id                  = module.network.vpc_id
  public_subnet_ids       = module.network.public_subnet_ids
  alb_security_group_id   = module.network.alb_security_group_id
  ecs_security_group_id   = module.network.ecs_security_group_id
  database_url_secret_arn = aws_secretsmanager_secret.database_url.arn
  redis_url_secret_arn    = aws_secretsmanager_secret.redis_url.arn
  cpu                     = var.backend_cpu
  memory                  = var.backend_memory
}

module "frontend" {
  source = "./modules/frontend"

  name_prefix  = var.name_prefix
  alb_dns_name = module.ecs.alb_dns_name
}
