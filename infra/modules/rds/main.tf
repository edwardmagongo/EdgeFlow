resource "aws_db_subnet_group" "this" {
  name       = "${var.name_prefix}-db"
  subnet_ids = var.subnet_ids

  tags = { Name = "${var.name_prefix}-db" }
}

# override_special excludes characters that need escaping inside a URL. The
# password lands in a postgres:// connection string, so a raw '/' or '@' would
# corrupt it.
resource "random_password" "db" {
  length           = 32
  special          = true
  override_special = "-_"
}

resource "aws_db_instance" "this" {
  identifier     = "${var.name_prefix}-postgres"
  engine         = "postgres"
  engine_version = "16"
  instance_class = var.instance_class

  allocated_storage = 20
  storage_type      = "gp3"
  storage_encrypted = true

  db_name  = "edgeflow"
  username = "edgeflow"
  password = random_password.db.result

  db_subnet_group_name   = aws_db_subnet_group.this.name
  vpc_security_group_ids = [var.security_group_id]
  publicly_accessible    = false
  multi_az               = false

  # One environment, no restore story in this phase. A final snapshot on
  # destroy would leave a billable artifact terraform destroy does not remove,
  # which the spec's acceptance criterion 6 forbids.
  skip_final_snapshot     = true
  backup_retention_period = 0

  apply_immediately = true

  tags = { Name = "${var.name_prefix}-postgres" }
}
