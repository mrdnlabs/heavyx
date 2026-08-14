ARG ARCH=aarch64
ARG VERSION=12.2.0
FROM axisecp/acap-native-sdk:${VERSION}-${ARCH}

COPY ./app /opt/app/
WORKDIR /opt/app
RUN . /opt/axis/acapsdk/environment-setup* && acap-build .
