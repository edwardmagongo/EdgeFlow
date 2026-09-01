#!/usr/bin/env bash
# Builds the dashboard against the deployed API and publishes it.
#
# VITE_API_BASE_URL is empty on purpose: CloudFront serves the SPA and proxies
# /v1/* to the ALB from the same origin, so relative paths are correct and no
# hostname is baked into the bundle.
set -euo pipefail

cd "$(dirname "$0")/.."

BUCKET="$(terraform -chdir=infra output -raw dashboard_bucket)"
DIST="$(terraform -chdir=infra output -raw cloudfront_distribution_id)"
DOMAIN="$(terraform -chdir=infra output -raw cloudfront_domain)"

echo "==> building"
(cd frontend && VITE_API_BASE_URL="" npm run build)

echo "==> syncing to s3://${BUCKET}"
# Hashed assets are immutable and cached hard; index.html must not be, or a
# deploy would keep serving the previous bundle until the cache expires.
aws s3 sync frontend/dist "s3://${BUCKET}" --delete \
  --exclude index.html --cache-control "public,max-age=31536000,immutable"
aws s3 cp frontend/dist/index.html "s3://${BUCKET}/index.html" \
  --cache-control "no-cache,no-store,must-revalidate"

echo "==> invalidating"
aws cloudfront create-invalidation --distribution-id "${DIST}" --paths '/*' >/dev/null

echo "==> deployed to https://${DOMAIN}"
