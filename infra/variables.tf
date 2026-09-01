variable "region" {
  description = "AWS region for every resource in this configuration."
  type        = string
  default     = "eu-west-1"
}

variable "name_prefix" {
  description = "Prefix for every named resource, so a second deployment cannot collide."
  type        = string
  default     = "edgeflow"
}

variable "vpc_cidr" {
  description = "CIDR block for the VPC."
  type        = string
  default     = "10.0.0.0/16"
}

variable "db_instance_class" {
  description = "RDS instance class."
  type        = string
  default     = "db.t4g.micro"
}

variable "redis_node_type" {
  description = "ElastiCache node type."
  type        = string
  default     = "cache.t4g.micro"
}

variable "backend_cpu" {
  description = "Fargate task CPU units."
  type        = number
  default     = 512
}

variable "backend_memory" {
  description = "Fargate task memory in MiB."
  type        = number
  default     = 1024
}
