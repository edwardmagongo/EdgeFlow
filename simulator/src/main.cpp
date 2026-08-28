#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <vector>
#include "device_client.hpp"
#include "edgeflow/simulator/config.hpp"

int main(int argc, char** argv) {
    edgeflow::simulator::Config config;
    try {
        config = edgeflow::simulator::parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-simulator: " << e.what() << '\n';
        return 1;
    }

    try {
        boost::asio::io_context io_context;
        std::vector<std::shared_ptr<edgeflow::simulator::DeviceClient>> clients;
        clients.reserve(config.device_count);

        for (std::size_t i = 0; i < config.device_count; ++i) {
            auto client = std::make_shared<edgeflow::simulator::DeviceClient>(
                io_context, config.host, config.port, static_cast<std::int64_t>(i),
                config.events_per_second_per_device);
            clients.push_back(client);
            client->start();
        }

        boost::asio::steady_timer stop_timer(io_context);
        stop_timer.expires_after(std::chrono::seconds(config.duration_seconds));
        stop_timer.async_wait([&](const boost::system::error_code&) {
            for (auto& client : clients) {
                client->stop();
            }
            io_context.stop();
        });

        std::cout << "edgeflow-simulator: connecting " << config.device_count
                  << " devices to " << config.host << ":" << config.port << " for "
                  << config.duration_seconds << "s\n";

        io_context.run();
        std::cout << "edgeflow-simulator: done\n";
    } catch (const std::exception& e) {
        std::cerr << "edgeflow-simulator: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
