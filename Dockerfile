FROM node:24-alpine
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev
COPY src ./src
COPY public ./public
COPY deploy/container-entrypoint.sh ./deploy/container-entrypoint.sh
RUN addgroup -S -g 10001 babytank && adduser -S -D -H -u 10001 -G babytank babytank && mkdir -p /data && chown babytank:babytank /data && chmod 0555 /app/deploy/container-entrypoint.sh
USER babytank
ENV HOST=0.0.0.0 PORT=8787 DATA_DIR=/data NODE_ENV=production COOKIE_SECURE=true
EXPOSE 8787
VOLUME ["/data"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
  CMD wget -q -T 5 -O /dev/null http://127.0.0.1:8787/healthz || exit 1
ENTRYPOINT ["/app/deploy/container-entrypoint.sh"]
