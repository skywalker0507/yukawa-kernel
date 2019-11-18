FROM ubuntu:xenial-20190720
MAINTAINER Neil Armstrong <narmstrong@baylibre.com>
LABEL Description=" This image is for testing Linux with bbci"

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq
RUN apt-get install -qq build-essential bison flex libncurses-dev libssl-dev libelf-dev bc device-tree-compiler kmod
RUN apt-get install -qq git python3 python3-yaml python3-jinja2 wget

# Setup system
RUN apt-get install -qq locales
RUN locale-gen en_US.UTF-8
RUN update-locale LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8

# Use en_US.UTF-8 locale
ENV LANG=en_US.UTF-8

# Install lavacli
RUN apt-get install -qq python3-pip
RUN pip3 install lavacli

# Install bbci
RUN git clone --depth 1 https://github.com/superna9999/bbci.git -b yukawa /root/bbci
