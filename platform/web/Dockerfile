FROM node:22-alpine AS builder
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY . .
ARG API_ORIGIN=http://localhost:8080
ENV API_ORIGIN=$API_ORIGIN
RUN npm run build

FROM node:22-alpine
WORKDIR /app
ENV NODE_ENV=production
# 把构建期的 API 源一并带进运行期镜像：Nitro 的 fetch 代理已在构建期把它烘进产物，
# 而 server/plugins/ws-proxy.ts 是运行时读的（优先读 WS_UPSTREAM，缺失时回落到这里），
# 免得「构建期一个地址、运行期一个地址」两处不同步时 WS 直接握手失败。
ARG API_ORIGIN=http://localhost:8080
ENV API_ORIGIN=$API_ORIGIN
COPY --from=builder /app/.output ./.output
COPY --from=builder /app/node_modules ./node_modules
EXPOSE 3000
CMD ["node", ".output/server/index.mjs"]
