output "repository_url" {
  value = aws_ecr_repository.this.repository_url
}

output "cluster_name" {
  value = aws_ecs_cluster.this.name
}

output "service_name" {
  value = aws_ecs_service.this.name
}

output "task_definition_family" {
  value = aws_ecs_task_definition.this.family
}

output "alb_dns_name" {
  value = aws_lb.this.dns_name
}

output "task_security_group_id" {
  value = var.ecs_security_group_id
}

output "subnet_ids" {
  value = var.public_subnet_ids
}

output "log_group_name" {
  value = aws_cloudwatch_log_group.this.name
}
