screenshot: screenshot.cc
	g++ -Wall -Werror -DUSE_GLOG \
	  `pkg-config --cflags --libs cairo gflags libglog x11` \
	  -o screenshot screenshot.cc

all: screenshot

clean:
	rm -f screenshot
