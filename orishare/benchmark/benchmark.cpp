//
// Created by yyx on 2024/1/18.
//

#include <iostream>
#include <chrono>
#include <map>
#include <memory>
#include <boost/program_options.hpp>
#include <random>
#include <vector>
#include "ResultWriter.h"
#include "omp.h"
#include "./competitors/MapInterface.h"
#include "./competitors/MapEmbed/MapEmbedInterface.h"
#include "./competitors/ShareHash/ShareHashInterface.h"
#include "./competitors/ShareHash/ShareHashSubtableInterface.h"
#include "./competitors/UnorderedMap/UnorderedMapInterface.h"
#include "./competitors/Cuckoo/CuckooMapInterface.h"
#include "./competitors/SparseHash/SparseHash.h"

struct configuration
{
    int datasetNumber;
    std::unique_ptr<MapInterface<uint64_t, uint64_t>> hashmap;
    int initRatio;
    double readRatio;
    double writeRatio;
    int threadNumber;
};

int main(int argc, char** argv)
{
    // int* a = new int[10];
    // a[20] = 5;  // 明确越界
    // std::cout<<a[20];
    //benchmark configuration
    struct configuration config;
    config.datasetNumber = 0;
    config.hashmap = nullptr;
    config.initRatio = 0.0;
    config.readRatio = 1.0;
    config.writeRatio = 0.0;
    config.threadNumber = 1;
    std::vector<std::thread> threads;
    std::string hashmapType;

    boost::program_options::options_description desc("Allowed options");
    desc.add_options()
        ("hashmap-type,t", boost::program_options::value<std::string>(), "Input hashmap type")
        ("dataset-size,d", boost::program_options::value<std::string>(), "Dataset size")
        ("init-ratio,i", boost::program_options::value<std::string>(), "Init ratio")
        ("read-ratio,r", boost::program_options::value<std::string>(), "Read ratio")
        ("thread-number,p", boost::program_options::value<std::string>(), "Thread number")
        ("seed", boost::program_options::value<uint64_t>(), "Optional reproducible dataset seed")
        ("result-dir", boost::program_options::value<std::string>()->default_value(SHAREHASH_RESULT_DIR), "CSV result directory")
        ("help", "Print help message");

    boost::program_options::variables_map vm;
    try
    {
        boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
        boost::program_options::notify(vm);
    }
    catch (const boost::program_options::error& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    if (vm.count("hashmap-type"))
    {
        hashmapType = vm["hashmap-type"].as<std::string>();
        std::cout << "Hashmap type: " << hashmapType << std::endl;

        if (hashmapType == "unorderedmap")
        {
            config.hashmap = std::make_unique<UnorderedMapInterface<uint64_t, uint64_t>>();
        }
        else if (hashmapType == "mapembed")
        {
            config.hashmap = std::make_unique<MapEmbedInterface<uint64_t, uint64_t>>();
        }
        else if (hashmapType == "sharehash")
        {
            config.hashmap = std::make_unique<ShareHashInterface<uint64_t, uint64_t>>();
        }
        else if (hashmapType == "sharehash_subtable")
        {
            config.hashmap = std::make_unique<ShareHashSubtableInterface<uint64_t, uint64_t>>();
        }
        else if (hashmapType == "cuckoohash")
        {
            config.hashmap = std::make_unique<CuckooInterface<uint64_t, uint64_t>>();
        }
        else if (hashmapType == "sparsehash")
        {
            config.hashmap = std::make_unique<SparseHashInterface<uint64_t, uint64_t>>();
        }
    }

    if (vm.count("dataset-size"))
    {
        std::string datasetSize = vm["dataset-size"].as<std::string>();
        std::cout << "Dataset size: " << datasetSize << std::endl;
        config.datasetNumber = std::stoi(datasetSize);
    }

    if (vm.count("init-ratio"))
    {
        std::string initRatio = vm["init-ratio"].as<std::string>();
        std::cout << "Init ratio: " << initRatio << std::endl;
        config.initRatio = std::stod(initRatio);
    }

    if (vm.count("read-ratio"))
    {
        std::string readRatio = vm["read-ratio"].as<std::string>();
        std::cout << "Read ratio: " << readRatio << std::endl;
        config.readRatio = std::stod(readRatio);
        config.writeRatio = 1.0 - config.readRatio;
    }

    if (vm.count("thread-number"))
    {
        std::string threadNumber = vm["thread-number"].as<std::string>();
        std::cout << "Thread number: " << threadNumber << std::endl;
        config.threadNumber = std::stoi(threadNumber);
    }

    if (vm.count("help"))
    {
        std::cout << desc << std::endl;
        return 0;
    }

    if (!config.hashmap || config.datasetNumber <= 0 || config.threadNumber <= 0) {
        std::cerr << "A supported hashmap, positive dataset size and positive thread count are required." << std::endl;
        return 1;
    }

    const auto memory_before_init = benchmark_results::sample_memory();
    //init index
    config.hashmap->init();
    const auto memory_after_init = benchmark_results::sample_memory();

    //generate data
    std::random_device rd;
    const uint64_t seed = vm.count("seed") ? vm["seed"].as<uint64_t>() : rd();
    std::mt19937_64 gen(seed);
    std::uniform_int_distribution<int64_t> dis(std::numeric_limits<int64_t>::min(),
                                               std::numeric_limits<int64_t>::max());
    std::vector<std::pair<int64_t, int64_t>> dataset;

    dataset.reserve(config.datasetNumber);
    //generate dataset
    for (int i = 0; i < config.datasetNumber; i++)
    {
        int64_t randomNum = dis(gen);
        dataset.push_back({randomNum, randomNum});
    }

    const auto memory_before_insert = benchmark_results::sample_memory();
    long long insert_successes = 0;
    long long lookup_hits = 0;

    //start time
    auto start = std::chrono::high_resolution_clock::now();


#pragma omp parallel num_threads(config.threadNumber) reduction(+:insert_successes)
    {
        //thread info
        auto thread_id = omp_get_thread_num();
        //        std::cout << thread_id << std::endl;
        int range = config.datasetNumber / config.threadNumber;
        int start = range * thread_id;
        int end = start + range;

        for (int i = start; i < end; i++)
        {
            insert_successes += config.hashmap->insert(dataset[i].first, dataset[i].second);
        }
    }
    //    //insert
    //    for(int i = 0 ; i < config.datasetNumber ; i++){
    //        config.hashmap->insert(dataset[i].first, dataset[i].second);
    //    }
    //stop timing
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    const double insert_seconds = duration.count();

    //print throughput
    long double throughput = config.datasetNumber / duration.count();
    std::cout << "Hashmap insert throughput: " << throughput << " elements/second" << std::endl;

    // Save an untimed insertion checkpoint even if the original query code aborts.
    const long long reported_memory_after_insert = config.hashmap->memory_consumption();
    const long long lock_memory_after_insert = config.hashmap->lock_memory_consumption();
    const auto memory_after_insert = benchmark_results::sample_memory();
    try {
        const benchmark_results::Result result {
            hashmapType, config.datasetNumber, config.threadNumber, seed,
            insert_successes, 0, insert_seconds, 0.0, reported_memory_after_insert,
            static_cast<long long>(dataset.size() * sizeof(dataset[0])),
            memory_before_init, memory_after_init, memory_before_insert, memory_after_insert,
            lock_memory_after_insert
        };
        benchmark_results::append_memory_csv(vm["result-dir"].as<std::string>(), result);
    } catch (const std::exception& error) {
        std::cerr << "Cannot save insertion memory results: " << error.what() << std::endl;
        return 1;
    }

    start = std::chrono::high_resolution_clock::now();

#pragma omp parallel num_threads(config.threadNumber) reduction(+:lookup_hits)
    {
        //thread info
        auto thread_id = omp_get_thread_num();
        //        std::cout << thread_id << std::endl;
        int range = config.datasetNumber / config.threadNumber;
        int start = range * thread_id;
        int end = start + range;
        uint64_t get_val = 0;

        for (int i = start; i < end; i++)
        {
            lookup_hits += config.hashmap->get(dataset[i].first, get_val);
        }
    }

    end = std::chrono::high_resolution_clock::now();
    duration = end - start;
    const double read_seconds = duration.count();

    //print throughput
    throughput = config.datasetNumber / duration.count();
    std::cout << "Hashmap read throughput: " << throughput << " elements/second" << std::endl;

    //memory consumption
    const long long reported_memory = config.hashmap->memory_consumption();
    std::cout << "Space consumption: " << reported_memory << "bytes" << std::endl;
    const long long lock_memory = config.hashmap->lock_memory_consumption();
    const auto memory_after_workload = benchmark_results::sample_memory();
    std::cout << "Index BPK (reported bytes/key): "
              << static_cast<double>(reported_memory) / config.datasetNumber << std::endl;
    std::cout << "Lock object bytes excluded from adjusted metrics: " << lock_memory << std::endl;
    std::cout << "Index BPK (reported, excluding lock object bytes): "
              << static_cast<double>(reported_memory - lock_memory) / config.datasetNumber << std::endl;
    std::cout << "Successful inserts: " << insert_successes
              << ", lookup hits: " << lookup_hits << std::endl;
    try {
        const benchmark_results::Result result {
            hashmapType, config.datasetNumber, config.threadNumber, seed,
            insert_successes, lookup_hits, insert_seconds, read_seconds, reported_memory,
            static_cast<long long>(dataset.size() * sizeof(dataset[0])),
            memory_before_init, memory_after_init, memory_before_insert, memory_after_workload,
            lock_memory
        };
        const auto path = benchmark_results::append_csv(vm["result-dir"].as<std::string>(), result);
        std::cout << "Results saved to: " << path << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "Cannot save benchmark results: " << error.what() << std::endl;
        return 1;
    }

    return 0;
}
