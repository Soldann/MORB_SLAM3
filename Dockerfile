
FROM ubuntu:20.04

########################################################################
# Running this docker to set up a shared directory and display with the host:
#
# To newly create and build this docker image:
# ============================================
#
# Create a directory <container_name>: 
#    $ mkdir <container_name>
# Copy this Dockerfile into that directory:
#    cp Dockerfile <container_name>/.
# Move to that directory:
#    $ cd <container_name>
# To build the docker file (might have to run with sudo 
#    $ sudo docker build -t <container_name> .
#
# To run the image, or run it again retaining its state 
# =====================================================
#    but also exporting display from the container and
#    sharing a directory between host and container:
#
# Allow other processes to share the display:
#    $ xhost +    #Allows or other processes to capture (show) the display
# Now run the docker (Usually $DISPLAY is :0) and allow use of the camera -- you may need sudo privalage
#    $ sudo docker run  -it  -e DISPLAY=$DISPLAY  -v /tmp/.X11-unix:/tmp/.X11-unix \
#                       --device /dev/video0 \
#                       -v /<path_to_a host_directory>/<directory_on_host>/:/<directory_path/name>/  <container_name>
#
# =======================================================
# Handy docker commands:
# List all the docker images
#    $ sudo docker ps -a  
# If the docker image is stopped (otherwise can skip the first command below if not stopped)
#    $ sudo docker start <container ID from ps -a above>
#    $ sudo docker attach <container ID from ps -a above>
########################################################################
# This is a docker file which will, from scratch:
#
#   * pull in all the dependencies needed for OpenCV 3.2 including python 2 dependencies
#   * pull in OpenCV 3.2 and opencv_contrib and build them
#       + executable files end up in opencv-3.2.0/build/bin
#   * pull in the Learning OpenCV 3 example code and build it
#       + executable files end up in Learning_OpenCV-3_examples/build
#   * To get to the top level directory, just type: cd
#
# If you just want to do this "by hand" in your home, replace the "RUN"s below with "sudo"
#
# This Docker uses the ubuntu 16.04 version of ffmpeg, which is older than the ones in my other dockerfiles.
# this shouldn't cause you any problems but definitely *DO NOT* use this for generating audiofiles / movies for redistribution.
#
# But it is somewhat less capable than the ones in the ffmpeg containers.
########################################################################

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ="America/New_York"

# Install minimal prerequisites (Ubuntu 18.04 as reference)
RUN apt update && apt install -y cmake g++ wget unzip git && mkdir opencv
# Download and unpack sources
WORKDIR "/opencv"
RUN wget -O opencv.zip https://github.com/opencv/opencv/archive/4.5.4.zip \
    && unzip opencv.zip
# Create build directory
RUN mkdir -p build && cd build \
# Configure
    && cmake  ../opencv-4.5.4 \
# Build
    && cmake --build . \
    && make install
    

#python
RUN apt install -y libpython2.7-dev ninja-build libeigen3-dev libsuitesparse-dev qtdeclarative5-dev qt5-qmake libqglviewer-dev-qt5 libceres-dev

#pangolin
RUN apt install -y libglew-dev ffmpeg libavcodec-dev libavutil-dev libavformat-dev libswscale-dev \
        libdc1394-22-dev libraw1394-dev \
        libjpeg-dev libpng-dev libtiff5-dev libopenexr-dev \
        doxygen python3-pydot python3-pydot-ng
RUN apt install -y graphviz 

WORKDIR "/Pangolin"
RUN git clone https://github.com/stevenlovegrove/Pangolin.git
#git checkout v0.7
#git submodule update --init --recursive

# install pangolin prerequisites
RUN apt install -y --no-install-suggests --no-install-recommends \
    libgl1-mesa-dev libwayland-dev libxkbcommon-dev wayland-protocols libegl1-mesa-dev \
    libc++-dev libglew-dev libeigen3-dev cmake g++ ninja-build \
    libjpeg-dev libpng-dev \
    libavcodec-dev libavutil-dev libavformat-dev libswscale-dev libavdevice-dev \
    libdc1394-22-dev libraw1394-dev libopenni-dev python3.9-dev python3-distutils

RUN cd Pangolin \
    && cmake -B build \
    && cmake --build build -j12

#cmake --build build -t pypangolin_pip_install


# morbslam
RUN apt install -y libboost-all-dev libssl-dev

WORKDIR "/MORB_SLAM"

RUN git clone https://github.com/Soldann/MORB_SLAM.git \
    && cd MORB_SLAM \
    && ./build.sh
