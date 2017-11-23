FROM ubuntu:16.04
LABEL maintainer mendonca.felippe@gmail.com

WORKDIR /opt
ADD service .
ADD libs/ /usr/lib/