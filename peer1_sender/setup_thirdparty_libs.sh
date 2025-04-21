mkdir -p third_party
cd third_party && git clone https://github.com/zaphoyd/websocketpp.git
cd third_party && git clone --recurse-submodules -j8 https://github.com/nlohmann/json.git