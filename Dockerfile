FROM node:24-alpine
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev
COPY src ./src
COPY public ./public
RUN addgroup -S -g 10001 babytank && adduser -S -D -H -u 10001 -G babytank babytank && mkdir -p /data && chown babytank:babytank /data
USER babytank
ENV HOST=0.0.0.0 PORT=8787 DATA_DIR=/data NODE_ENV=production COOKIE_SECURE=true
EXPOSE 8787
VOLUME ["/data"]
CMD ["node", "src/server.mjs"]
