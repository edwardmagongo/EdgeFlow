# sslmode is required, not optional. RDS PostgreSQL 15 and later ship
# rds.force_ssl = 1 in the default parameter group, so an unencrypted
# connection is refused outright:
#
#   FATAL: no pg_hba.conf entry for host "...", user "edgeflow",
#          database "edgeflow", no encryption
#
# This does not reproduce against the docker-compose Postgres, which accepts
# unencrypted connections, so it only appears once the backend talks to RDS.
#
# no-verify rather than require: pg maps sslmode=require to ssl: true, and Node
# then validates the server certificate against its own CA bundle. RDS
# certificates are signed by the Amazon RDS CA, which is not in that bundle, so
# require fails on an unknown issuer. no-verify encrypts the connection, which
# is what force_ssl demands, without verifying the issuer.
#
# The trade-off is worth naming: this encrypts in transit but does not
# authenticate the server, so it does not defend against an in-VPC MITM. The
# instance sits in a private subnet reachable only from the ECS security group,
# and that isolation is what carries the risk. Bundling the RDS CA into the
# image and moving to verify-full is the stronger option if that ever weakens.
output "connection_url" {
  value     = "postgres://${aws_db_instance.this.username}:${random_password.db.result}@${aws_db_instance.this.endpoint}/${aws_db_instance.this.db_name}?sslmode=no-verify"
  sensitive = true
}

output "endpoint" {
  value = aws_db_instance.this.endpoint
}
