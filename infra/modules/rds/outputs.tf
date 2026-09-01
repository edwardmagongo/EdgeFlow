output "connection_url" {
  value     = "postgres://${aws_db_instance.this.username}:${random_password.db.result}@${aws_db_instance.this.endpoint}/${aws_db_instance.this.db_name}"
  sensitive = true
}

output "endpoint" {
  value = aws_db_instance.this.endpoint
}
