output "connection_url" {
  value     = "redis://${aws_elasticache_cluster.this.cache_nodes[0].address}:${aws_elasticache_cluster.this.port}"
  sensitive = true
}
