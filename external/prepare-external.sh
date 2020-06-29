#!/bin/bash

# change to external subdir if we were called from somewhere else
cd $(dirname $0)

echo "Preparing Simple-Web-Server external sources..."

if [ ! -d Simple-Web-Server ]; then
   git clone https://gitlab.com/eidheim/Simple-Web-Server.git
fi

cd Simple-Web-Server && \
git fetch --all && \
git checkout v3.0.2 && \
git status && \
echo "All done."
