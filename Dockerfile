FROM centos:7
ARG DEVTOOLSET_VERSION=11
ARG NV_KERNEL_VERSION
### Install basic requirements
RUN yum install -y centos-release-scl
RUN yum install -y devtoolset-${DEVTOOLSET_VERSION}
RUN yum install -y wget
ARG CMAKE_VERSION=3.23.3
RUN cd /usr/local && wget --quiet https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz && \
   tar zxf cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz && \
   rm cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz
ENV PATH /usr/local/cmake-${CMAKE_VERSION}-linux-x86_64/bin:$PATH
RUN mkdir -p /nv-resource-controller/build
COPY . /nv-resource-controller
RUN cd /nv-resource-controller/build && \
  source /opt/rh/devtoolset-11/enable && \
  cmake .. -DCMAKE_BUILD_TYPE=Release -DNV_KERNEL_VERSION=${NV_KERNEL_VERSION} && \
  make
