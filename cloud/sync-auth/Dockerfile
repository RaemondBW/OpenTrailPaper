# Cloud Run image. No npm install: the service has no dependencies, so the
# image is the Node runtime plus one file.
FROM node:20-alpine
WORKDIR /app
COPY package.json server.js ./
ENV NODE_ENV=production
USER node
CMD ["node", "server.js"]
