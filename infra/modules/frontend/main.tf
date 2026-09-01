resource "random_id" "bucket" {
  byte_length = 4
}

resource "aws_s3_bucket" "site" {
  bucket        = "${var.name_prefix}-dashboard-${random_id.bucket.hex}"
  force_destroy = true
}

resource "aws_s3_bucket_public_access_block" "site" {
  bucket                  = aws_s3_bucket.site.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

# OAC, not the older OAI. The bucket stays private and CloudFront signs its
# origin requests.
resource "aws_cloudfront_origin_access_control" "site" {
  name                              = "${var.name_prefix}-oac"
  origin_access_control_origin_type = "s3"
  signing_behavior                  = "always"
  signing_protocol                  = "sigv4"
}

data "aws_iam_policy_document" "site" {
  statement {
    actions   = ["s3:GetObject"]
    resources = ["${aws_s3_bucket.site.arn}/*"]

    principals {
      type        = "Service"
      identifiers = ["cloudfront.amazonaws.com"]
    }

    condition {
      test     = "StringEquals"
      variable = "AWS:SourceArn"
      values   = [aws_cloudfront_distribution.this.arn]
    }
  }
}

resource "aws_s3_bucket_policy" "site" {
  bucket = aws_s3_bucket.site.id
  policy = data.aws_iam_policy_document.site.json
}

# AWS-managed policy IDs, stable across accounts.
locals {
  caching_disabled_policy_id       = "4135ea2d-6df8-44a3-9df3-4b5a84be39ad"
  caching_optimized_policy_id      = "658327ea-f89d-4fab-a63d-7e88639e58f6"
  all_viewer_except_host_policy_id = "b689b0a8-53d0-40ab-baf2-68738e2966ac"
}

# Serves index.html for client-side routes so the SPA can render them, while
# leaving real asset requests alone -- a missing /assets/*.js must still 404
# rather than return HTML with status 200.
#
# Attached to the S3 behavior only. This replaces the custom_error_response
# approach, which CloudFront applies across the whole distribution and which
# therefore also rewrote the API's 403s and 404s into 200s.
resource "aws_cloudfront_function" "spa_router" {
  name    = "${var.name_prefix}-spa-router"
  runtime = "cloudfront-js-2.0"
  comment = "SPA deep-link fallback for the dashboard origin"
  publish = true

  code = <<-EOT
    function handler(event) {
      var request = event.request;
      var uri = request.uri;
      // Anything whose last path segment carries an extension is a real file.
      // Everything else is a client-side route.
      var lastSegment = uri.substring(uri.lastIndexOf('/') + 1);
      if (lastSegment.indexOf('.') === -1) {
        request.uri = '/index.html';
      }
      return request;
    }
  EOT
}

resource "aws_cloudfront_distribution" "this" {
  enabled             = true
  default_root_object = "index.html"
  comment             = "${var.name_prefix} dashboard and API"

  origin {
    origin_id                = "s3"
    domain_name              = aws_s3_bucket.site.bucket_regional_domain_name
    origin_access_control_id = aws_cloudfront_origin_access_control.site.id
  }

  origin {
    origin_id   = "alb"
    domain_name = var.alb_dns_name

    custom_origin_config {
      http_port  = 80
      https_port = 443
      # The ALB has no certificate: there is no custom domain and therefore no
      # ACM cert. CloudFront terminates TLS at the edge and talks plain HTTP to
      # the origin, which is what makes the dashboard same-origin with the API
      # and sidesteps mixed-content blocking and CORS together.
      origin_protocol_policy = "http-only"
      origin_ssl_protocols   = ["TLSv1.2"]
    }
  }

  default_cache_behavior {
    target_origin_id       = "s3"
    viewer_protocol_policy = "redirect-to-https"
    allowed_methods        = ["GET", "HEAD", "OPTIONS"]
    cached_methods         = ["GET", "HEAD"]
    cache_policy_id        = local.caching_optimized_policy_id

    # Text assets are worth compressing and this costs nothing; the dashboard
    # bundle is JS and CSS.
    compress = true

    # SPA deep links are resolved here rather than by a custom_error_response,
    # because those are configured per DISTRIBUTION, not per behavior: a 403 or
    # 404 from the ALB behind /v1/* would have been rewritten to index.html
    # with status 200 as well. The sink branches on status code, so a 404
    # arriving as 200 would read as "batch stored" and drop the batch silently.
    # A function attaches to one behavior, so the fallback cannot reach the API.
    function_association {
      event_type   = "viewer-request"
      function_arn = aws_cloudfront_function.spa_router.arn
    }
  }

  ordered_cache_behavior {
    path_pattern           = "/v1/*"
    target_origin_id       = "alb"
    viewer_protocol_policy = "redirect-to-https"
    allowed_methods        = ["GET", "HEAD", "OPTIONS", "PUT", "POST", "PATCH", "DELETE"]
    cached_methods         = ["GET", "HEAD"]

    # API responses must never be cached, and the origin needs the viewer's
    # headers and query string -- GET /v1/events paginates on a cursor query
    # parameter.
    cache_policy_id          = local.caching_disabled_policy_id
    origin_request_policy_id = local.all_viewer_except_host_policy_id
  }

  restrictions {
    geo_restriction {
      restriction_type = "none"
    }
  }

  viewer_certificate {
    cloudfront_default_certificate = true
  }
}
