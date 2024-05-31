#!/bin/bash

# https://stackoverflow.com/questions/24112727/relative-paths-based-on-file-location-instead-of-current-working-directory
parent_path=$( cd "$(dirname "${BASH_SOURCE[0]}")" ; pwd -P )
cd "$parent_path" # change directories so working directory is where the script is

check_install_packages() {
	MISSING_PACKAGES=""

	set +e
	for PACKAGE_NAME in "$@"; do
		# check if the package is installed
		dpkg -s $PACKAGE_NAME &> /dev/null

		if [ $? -eq 0 ]; then
			# echo "$PACKAGE_NAME is already installed."
      pass=""
		else
			MISSING_PACKAGES="$MISSING_PACKAGES $PACKAGE_NAME"
		fi
	done
	set -e

	if [ -z "$MISSING_PACKAGES" ]; then
      pass=""
      # echo "All apt package dependencies are already installed."
    else
      echo "Installing missing apt packages: $MISSING_PACKAGES"
		sudo apt update
		sudo apt install -y $MISSING_PACKAGES
  fi
}

check_pip(){
  check_install_packages python3-pip
  # python3 -c "import pkg_resources; pkg_resources.get_distribution('$package_name')" 1>/dev/null
  MISSING_PACKAGES=""

	set +e
	for PACKAGE_NAME in "$@"; do
		# check if the package is installed
		python3 -c "import pkg_resources; pkg_resources.get_distribution('$PACKAGE_NAME')" 1>/dev/null 2>&1

		if [ $? -eq 0 ]; then
			# echo "$PACKAGE_NAME is already installed."
      pass=""
		else
			MISSING_PACKAGES="$MISSING_PACKAGES $PACKAGE_NAME"
		fi
	done
	set -e

	if [ -z "$MISSING_PACKAGES" ]; then
      pass=""
      # echo "All apt package dependencies are already installed."
    else
      echo "Installing missing pip packages: $MISSING_PACKAGES"
		# sudo apt update
		python3 -m pip install $MISSING_PACKAGES
  fi
}

do_vcpkg_prereqs(){
  check_install_packages bison autoconf libtool curl pkg-config libxi-dev libxtst-dev libxrandr-dev nasm libgl-dev libxmu-dev libglu1-mesa-dev libudev-dev
  check_pip jinja2
  set +e
}



# Initialize lists
cmake_args=()
j_arg="-j$(nproc)"
g_arg=""
vcpkg_arg=""
# Iterate over arguments
for arg in "$@"; do
  if [[ $arg =~ ^-j[0-9]+$ ]]; then
    j_arg="$arg"
  elif [[ $arg =~ ^-G[0-9a-zA-Z]+$ ]]; then
    g_arg="$arg"
  elif [[ $arg =~ ^-DCMAKE_TOOLCHAIN_FILE ]]; then
    vcpkg_arg="found"
  else
    cmake_args+=("$arg")
  fi
done
# Check if the g_arg is empty (non-user specified generator)
if [ -z "$g_arg" ]; then
    # select ninja if available, make otherwise
    if which ninja >/dev/null 2>&1; then
        g_arg="-GNinja"
    elif which make >/dev/null 2>&1; then
        g_arg="-GMake"
    else
        echo "Please either install Ninja (preffered), Make, or specify an installed Generator?"
        echo "    For ninja `sudo apt install ninja-build`"
        echo "    For make  `sudo apt install build-essential`"
        exit -1
    fi
fi
if [ -f "vcpkgLoc.txt" ]; then
  vcpkg_arg=$(awk '{$1=$1};1' vcpkgLoc.txt) # this reads the first line and removes leading & trailing whitespace
  if [ ! -f "${vcpkg_arg}" ]; then
    vcpkg_arg=""
    echo "The vcpkg toolchain file was removed! Replace it or delete ${parent_path}/vcpkgLoc.txt and try again"
    exit -1
  fi
fi

if [ ! -f "Vocabulary/ORBvoc.txt" ]; then
    cd Vocabulary
    echo "Extracting vocabulary..."
    tar -xf ORBvoc.txt.tar.gz
    echo "ORB Vocabulary extracted"
    cd $parent_path
else
    echo "ORB Vocabulary already extracted"
fi

configCompleteFile='config-finished.bool'
# https://unix.stackexchange.com/questions/31414/how-can-i-pass-a-command-line-argument-into-a-shell-script
if [ ! -d "build" ] || [ ! -f "build/${configCompleteFile}" ]; then
        echo 'Performing first time configuration...'
        echo "Workers: ${j_arg}    Generator: ${g_arg}"
        if test -n "$vcpkg_arg"; then
          echo "With VCPKG"
          if [[ "$vcpkg_arg" != "found" ]]; then
            vcpkg_arg="-DCMAKE_TOOLCHAIN_FILE=${vcpkg_arg}"
          else
            vcpkg_arg=""
          fi
          do_vcpkg_prereqs
        fi
        echo "User Flags: ${cmake_args[@]}"
        mkdir build 2> /dev/null
        cd build
        # https://unix.stackexchange.com/questions/31414/how-can-i-pass-a-command-line-argument-into-a-shell-script
        cmake .. ${g_arg} "${vcpkg_arg}" "${cmake_args[@]}" # pass arguments on to cmake
        if [ $? -ne 0 ]; then
                rm "${configCompleteFile}" 2> /dev/null
                cd ..
                echo "Configuration was not successful"
                exit 1
        fi
        touch "${configCompleteFile}"
        echo -e "    Generator: ${g_arg}\nUser Flags: ${cmake_args[@]}" > "${configCompleteFile}"
else
        echo 'Already configured'
        cd build
        config=$(cat "${configCompleteFile}")
        echo "Workers: ${j_arg}${config}"
fi

if [ $? -ne 0 ]; then
	cd ..
	echo "Configuration was not successful"
	exit 1
fi
if test -n "$vcpkg_arg"; then
  do_vcpkg_prereqs
fi
echo "Building..."
cmake --build . -- ${j_arg}
if [ $? -ne 0 ]; then
        cd ..
        echo "Build was not successful"
        exit 2
fi
sudo cmake --install .
if [ $? -ne 0 ]; then
        cd ..
        echo "Install was not successful"
        exit 3
fi