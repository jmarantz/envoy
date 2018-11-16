#!/bin/bash


(set -x; doxygen tools/doxygen.cfg -w html >& /tmp/doxygen.log)
echo Doxygen generate complete: $SECONDS seconds.

# doxygen.cfg directs OUTPUT_DIRECTORY to /tmp/envoy-doxygen, so run
# an HTTP server there and show the output in chrome.
port=18238
cd /tmp/envoy-doxygen
python -m SimpleHTTPServer $port &
google-chrome http://localhost:$port/html/annotated.html
