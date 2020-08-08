#!/bin/bash

REQUIRED_TAG="v3.0.2"

# change to external subdir if we were called from somewhere else
cd $(dirname $0)

# clone if directory does not exist yet
if [ ! -d Simple-Web-Server ]; then
   echo "Cloning http server git repo..."
   git clone https://gitlab.com/eidheim/Simple-Web-Server.git
   if [ $? -ne 0 ]; then
      exit 1
   fi
fi

# directory exists, check if we already have the right tag.
# (this is the fast path for dependency calls from Makefile)
cd Simple-Web-Server && \
   CURRENT_TAG="$(git describe --tags)" && \
   if [ "$CURRENT_TAG" = "$REQUIRED_TAG" ]; then
      # All good, nothing to do
      exit 0;
   fi && \
   cd -

# we are not at the right tag, so try to check it out.
# (fetching is relevant in case we update to a new required tag.)
echo "Checking out http server tag ${REQUIRED_TAG}..."

cd Simple-Web-Server && \
   git fetch -q --all && \
   git checkout -q ${REQUIRED_TAG} && \
   echo "DONE: HTTP server sources prepared." && \
   exit 0

# something went wrong if we got here
echo "ERROR: HTTP server source preparation failed."
exit 1
