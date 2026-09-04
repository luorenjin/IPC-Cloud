FROM golang:1.25-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -o /out/ipcsim .

FROM alpine:3.20
RUN apk add --no-cache ca-certificates
COPY --from=build /out/ipcsim /usr/local/bin/ipcsim
COPY assets /assets
ENTRYPOINT ["ipcsim"]
