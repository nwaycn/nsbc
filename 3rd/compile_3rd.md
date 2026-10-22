
cd 3rd/

git clone -b v2.6.0  https://gitee.com/taniey/libsrtp.git
cd libsrtp


mkdir build && cd build
cmake .. \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DENABLE_OPENSSL=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) && sudo make install && sudo ldconfig

git clone https://gitee.com/opensource-release/bcg729.git

