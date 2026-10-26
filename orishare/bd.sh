cd /data00/home/wangmingze.886/test_bishe/sharehash-learn
rm -rf build
mkdir build && cd build
#cmake ..
cmake -DUSE_TSAN=ON ..
make -j$(nproc)

./benchmark -t cuckoohash -d 100000 -p 2

#rm -rf ./build
##cmake -B build -S . -DUSE_ASAN=ON
##cmake --build build -j
#
#mkdir build && cd build
##cmake .. -DUSE_ASAN=ON
#cmake ..
#### 启用 ASan
####cmake -DUSE_ASAN=ON -B build -S ..
###
#### 启用 TSan
####cmake -DUSE_TSAN=ON -B build -S .
#make -j$(nproc)
