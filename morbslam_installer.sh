#apt update for good measure
sudo apt update


#python
sudo apt install libpython2.7-dev

sudo apt install -y ninja-build
#g20
sudo apt install -y libeigen3-dev
sudo apt install -y libsuitesparse-dev qtdeclarative5-dev qt5-qmake libqglviewer-dev-qt5
sudo apt install -y libceres-dev

#pangolin
sudo apt install -y libglew-dev
sudo apt install -y libpython2.7-dev
sudo apt install -y ffmpeg libavcodec-dev libavutil-dev libavformat-dev libswscale-dev \
        libdc1394-22-dev libraw1394-dev \
        libjpeg-dev libpng-dev libtiff5-dev libopenexr-dev
#sudo apt install -y libavcodec-ffmpeg-dev
sudo apt install -y libeigen3-dev \
        doxygen python3-pydot python3-pydot-ng
sudo apt install -y graphviz # after python-pydot and python-pydot-ng

folder=${PWD##*/}
folder=${result:-/}
if [ "$folder" == "MORB_SLAM" ]; then
	cd ..
fi

git clone https://github.com/stevenlovegrove/Pangolin.git
cd Pangolin
#git checkout v0.7
#git submodule update --init --recursive

./scripts/install_prerequisites.sh -m all 


cmake -B build
cmake --build build -j12

cd ..


#cmake --build build -t pypangolin_pip_install


# morbslam
sudo apt install -y libboost-all-dev libssl-dev
chmod +x build.sh
./build.sh -j$(nproc)
