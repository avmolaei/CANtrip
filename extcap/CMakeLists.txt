add_executable(can2pcap can2pcap.cpp)
target_link_libraries(can2pcap PRIVATE cantrip_common)
cantrip_target_options(can2pcap)
