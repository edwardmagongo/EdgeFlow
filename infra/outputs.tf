output "vpc_id" {
  value = module.network.vpc_id
}

output "ecr_repository_url" {
  value = module.ecs.repository_url
}

output "ecs_cluster_name" {
  value = module.ecs.cluster_name
}

output "ecs_service_name" {
  value = module.ecs.service_name
}

output "task_definition_family" {
  value = module.ecs.task_definition_family
}

output "task_subnet_ids" {
  value = module.ecs.subnet_ids
}

output "task_security_group_id" {
  value = module.ecs.task_security_group_id
}

output "alb_dns_name" {
  value = module.ecs.alb_dns_name
}

output "log_group_name" {
  value = module.ecs.log_group_name
}

output "dashboard_bucket" {
  value = module.frontend.bucket_name
}

output "cloudfront_distribution_id" {
  value = module.frontend.distribution_id
}

output "cloudfront_domain" {
  value = module.frontend.domain_name
}

output "region" {
  value = var.region
}
