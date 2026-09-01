resource "aws_elasticache_subnet_group" "this" {
  name       = "${var.name_prefix}-redis"
  subnet_ids = var.subnet_ids
}

# No AUTH token, per the spec's security posture: the backend's REDIS_URL
# handling has no password path, and network isolation via security group is
# the only control. Adding AUTH later is additive to this module and the
# secret, not a redesign.
resource "aws_elasticache_cluster" "this" {
  cluster_id           = "${var.name_prefix}-redis"
  engine               = "redis"
  engine_version       = "7.1"
  node_type            = var.node_type
  num_cache_nodes      = 1
  parameter_group_name = "default.redis7"
  port                 = 6379

  subnet_group_name  = aws_elasticache_subnet_group.this.name
  security_group_ids = [var.security_group_id]

  tags = { Name = "${var.name_prefix}-redis" }
}
