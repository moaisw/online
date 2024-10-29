#! /bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# -- Available env vars --
# * DOCKER_HUB_REPO - which Docker Hub repo to use
# * DOCKER_HUB_TAG  - which Docker Hub tag to create
# * CORE_BRANCH  - which branch to build in core
# * COLLABORA_ONLINE_REPO - which git repo to clone online from
# * COLLABORA_ONLINE_BRANCH - which branch to build in online
# * CORE_BUILD_TARGET - which make target to run (in core repo)
# * ONLINE_EXTRA_BUILD_OPTIONS - extra build options for online
# * NO_DOCKER_IMAGE - if set, don't build the docker image itself, just do all the preps

# check we can sudo without asking a pwd
echo "Trying if sudo works without a password"
echo
echo "If you get a password prompt now, break, and fix your setup using 'sudo visudo'; add something like:"
echo "yourusername ALL=(ALL) NOPASSWD: /sbin/setcap"
echo
sudo echo "works"

echo "Installing dependencies"
sudo apt install -y dialog
sudo apt install -y libpoco-dev python3-polib libcap-dev npm \
                    libpam-dev libzstd-dev wget git build-essential libtool \
                    libcap2-bin python3-lxml libpng-dev libcppunit-dev \
                    pkg-config fontconfig chromium

# Check env variables
if [ -z "$DOCKER_HUB_REPO" ]; then
  DOCKER_HUB_REPO="pic.dip-caceres.es:5443/dipccdev/bop/collabora-online"
  
fi;
if [ -z "$DOCKER_HUB_TAG" ]; then
  DOCKER_HUB_TAG="latest-tls"
fi;
echo "Using Docker Hub Repository: '$DOCKER_HUB_REPO' with tag '$DOCKER_HUB_TAG'."

if [ -z "$CORE_BRANCH" ]; then
  CORE_BRANCH="co-24.04"
fi;
echo "Building core branch '$CORE_BRANCH'"

if [ -z "$COLLABORA_ONLINE_REPO" ]; then
  COLLABORA_ONLINE_REPO="https://github.com/moaisw/online.git"
fi;
if [ -z "$COLLABORA_ONLINE_BRANCH" ]; then
  COLLABORA_ONLINE_BRANCH="master"
fi;
echo "Building online branch '$COLLABORA_ONLINE_BRANCH' from '$COLLABORA_ONLINE_REPO'"

if [ -z "$CORE_BUILD_TARGET" ]; then
  CORE_BUILD_TARGET=""
fi;
echo "LOKit (core) build target: '$CORE_BUILD_TARGET'"

SRCDIR=$(realpath `dirname $0`)
INSTDIR="$SRCDIR/instdir"

if [ -z "$(lsb_release -si)" ]; then
  echo "WARNING: Unable to determine your distribution"
  echo "(Is lsb_release installed?)"
  echo "Using Ubuntu Dockerfile."
  HOST_OS="Ubuntu"
else
  HOST_OS=$(lsb_release -si)
fi
if ! [ -e "$SRCDIR/$HOST_OS" ]; then
  echo "There is no suitable Dockerfile for your host system: $HOST_OS."
  echo "Please fix this problem and re-run $0"
  exit 1
fi
BUILDDIR="$SRCDIR/builddir"

cd "$BUILDDIR"

# Create new docker image
if [ -z "$NO_DOCKER_IMAGE" ]; then
  echo "### Create Docker image ###"
  cd "$SRCDIR"
  cp ../from-packages/scripts/start-collabora-online.sh .
  cp coolwsd.xml.ori "$INSTDIR"/etc/coolwsd/coolwsd.xml
  sudo docker build --no-cache -t pic.dip-caceres.es:5443/dipccdev/bop/collabora-online:latest-tls -f $HOST_OS . || exit 1
  echo "gJXkvypXqNDdzxA4m4aD" | sudo docker login pic.dip-caceres.es:5443 -u dockerregistry --password-stdin
  sudo docker push pic.dip-caceres.es:5443/dipccdev/bop/collabora-online:latest-tls
  echo "Skipping docker image build"
fi;
