FROM node:22-alpine AS builder
WORKDIR /app
COPY package.json ./
RUN npm install
COPY . .
ARG API_ORIGIN=http://localhost:8080
ENV API_ORIGIN=$API_ORIGIN
RUN npm run build

FROM node:22-alpine
WORKDIR /app
ENV NODE_ENV=production
COPY --from=builder /app/.output ./.output
COPY --from=builder /app/node_modules ./node_modules
EXPOSE 3000
CMD ["node", ".output/server/index.mjs"]
