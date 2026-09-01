# Node 22 matches CI's actions/setup-node@v4 version in .github/workflows/ci.yml.
FROM node:22-alpine AS builder
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json ./
COPY src ./src
RUN npm run build

FROM node:22-alpine AS runtime
WORKDIR /app
ENV NODE_ENV=production

COPY package.json package-lock.json ./
RUN npm ci --omit=dev && npm cache clean --force

COPY --from=builder /app/dist ./dist
# migrate.js resolves ../migrations relative to itself, and the migration ECS
# task runs `npm run migrate` against this same image. Both directories are
# required, not conveniences.
COPY scripts ./scripts
COPY migrations ./migrations

# node:alpine ships an unprivileged `node` user. Nothing here writes to disk.
USER node

EXPOSE 3000

# Exec form, deliberately. A shell form (CMD npm start) makes node a child of
# /bin/sh, which does not forward SIGTERM -- ECS would then SIGKILL the task on
# stopTimeout and Task 1's shutdown hooks would never run.
CMD ["node", "dist/main.js"]
