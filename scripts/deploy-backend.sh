#!/usr/bin/env bash
# Deploys the backend: build, push under an immutable git-SHA tag, migrate, roll.
#
# Migrations run on EVERY deploy, not only the first. The design spec's runbook
# ran them once in step 3 and omitted them from step 4, so a later migration
# would be silently skipped and surface as runtime SQL errors against a schema
# that no longer matches the code.
#
# The tag is the git SHA, never :latest. A mutable tag makes it impossible to
# say which build is running or to roll back to the previous one.
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ -n "$(git status --porcelain)" ]]; then
  echo "error: working tree is dirty; the image tag would not identify its source" >&2
  exit 1
fi

SHA="$(git rev-parse --short HEAD)"
REPO="$(terraform -chdir=infra output -raw ecr_repository_url)"
CLUSTER="$(terraform -chdir=infra output -raw ecs_cluster_name)"
SERVICE="$(terraform -chdir=infra output -raw ecs_service_name)"
FAMILY="$(terraform -chdir=infra output -raw task_definition_family)"
SUBNETS="$(terraform -chdir=infra output -json task_subnet_ids | tr -d '[]"' )"
SG="$(terraform -chdir=infra output -raw task_security_group_id)"
REGION="$(terraform -chdir=infra output -raw region 2>/dev/null || echo eu-west-1)"
ACCOUNT="${REPO%%.*}"

echo "==> building ${REPO}:${SHA}"
docker build --platform linux/amd64 -t "${REPO}:${SHA}" backend/

echo "==> pushing"
aws ecr get-login-password --region "${REGION}" \
  | docker login --username AWS --password-stdin "${ACCOUNT}.dkr.ecr.${REGION}.amazonaws.com"
docker push "${REPO}:${SHA}"

echo "==> registering task definition revision for ${SHA}"
CURRENT="$(aws ecs describe-task-definition --task-definition "${FAMILY}" --region "${REGION}")"
NEW_DEF="$(echo "${CURRENT}" | python3 -c '
import json, sys
d = json.load(sys.stdin)["taskDefinition"]
d["containerDefinitions"][0]["image"] = sys.argv[1]
for k in ("taskDefinitionArn", "revision", "status", "requiresAttributes",
          "compatibilities", "registeredAt", "registeredBy"):
    d.pop(k, None)
print(json.dumps(d))
' "${REPO}:${SHA}")"
TASKDEF_ARN="$(aws ecs register-task-definition --region "${REGION}" \
  --cli-input-json "${NEW_DEF}" --query 'taskDefinition.taskDefinitionArn' --output text)"

echo "==> running migrations against ${TASKDEF_ARN}"
TASK_ARN="$(aws ecs run-task --region "${REGION}" \
  --cluster "${CLUSTER}" \
  --task-definition "${TASKDEF_ARN}" \
  --launch-type FARGATE \
  --network-configuration "awsvpcConfiguration={subnets=[${SUBNETS}],securityGroups=[${SG}],assignPublicIp=ENABLED}" \
  --overrides '{"containerOverrides":[{"name":"backend","command":["npm","run","migrate"]}]}' \
  --query 'tasks[0].taskArn' --output text)"

aws ecs wait tasks-stopped --region "${REGION}" --cluster "${CLUSTER}" --tasks "${TASK_ARN}"

EXIT_CODE="$(aws ecs describe-tasks --region "${REGION}" --cluster "${CLUSTER}" --tasks "${TASK_ARN}" \
  --query 'tasks[0].containers[0].exitCode' --output text)"
if [[ "${EXIT_CODE}" != "0" ]]; then
  echo "error: migration task exited ${EXIT_CODE}; not deploying" >&2
  exit 1
fi

echo "==> rolling the service"
aws ecs update-service --region "${REGION}" \
  --cluster "${CLUSTER}" --service "${SERVICE}" \
  --task-definition "${TASKDEF_ARN}" >/dev/null

aws ecs wait services-stable --region "${REGION}" --cluster "${CLUSTER}" --services "${SERVICE}"

echo "==> deployed ${SHA}"
