variable "name_prefix" {
  type = string
}

variable "vpc_id" {
  type = string
}

variable "public_subnet_ids" {
  type = list(string)
}

variable "alb_security_group_id" {
  type = string
}

variable "ecs_security_group_id" {
  type = string
}

variable "database_url_secret_arn" {
  type = string
}

variable "redis_url_secret_arn" {
  type = string
}

variable "cpu" {
  type = number
}

variable "memory" {
  type = number
}
