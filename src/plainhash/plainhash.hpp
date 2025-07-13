#pragma once

#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include <fmt/color.h>
#include <fmt/format.h>
#include <libbase64.h>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "src/core/db.hpp"
#include "src/core/helper.hpp"
#include "src/core/types.hpp"

namespace ucsb::plain
{

    std::vector<std::byte> decodeBase64(const std::string& input)
    {
        size_t out_len = 0;
        std::vector<std::byte> out(input.size()); // Max size is safe here

        base64_decode(input.data(), input.size(), reinterpret_cast<char*>(out.data()), &out_len, 0);

        out.resize(out_len);
        return out;
    }

    std::string encodeBase64(std::span<std::byte> data)
    {
        size_t out_len = 0;
        size_t max_out = 4 * ((data.size() + 2) / 3);
        std::vector<char> out(max_out);

        base64_encode(reinterpret_cast<const char*>(data.data()), data.size(), out.data(), &out_len, 0);

        return std::string(out.data(), out_len);
    }

    namespace fs = ucsb::fs;

    using key_t = ucsb::key_t;
    using keys_spanc_t = ucsb::keys_spanc_t;
    using value_span_t = ucsb::value_span_t;
    using value_spanc_t = ucsb::value_spanc_t;
    using values_span_t = ucsb::values_span_t;
    using values_spanc_t = ucsb::values_spanc_t;
    using value_lengths_spanc_t = ucsb::value_lengths_spanc_t;
    using operation_status_t = ucsb::operation_status_t;
    using operation_result_t = ucsb::operation_result_t;
    using db_hints_t = ucsb::db_hints_t;
    using transaction_t = ucsb::transaction_t;


    class plainhash_t : public ucsb::db_t
    {
    public:
        inline plainhash_t() : map_(std::make_unique<std::unordered_map<key_t, value_spanc_t>>()) {}
        ~plainhash_t() { plainhash_t::close(); }

        void set_config(fs::path const& config_path, fs::path const& main_dir_path,
                        std::vector<fs::path> const& storage_dir_paths, db_hints_t const& hints) override;
        bool open(std::string& error) override;
        void save_as_json() const;
        void close() override;

        std::string info() override;

        operation_result_t upsert(key_t key, value_spanc_t value) override;
        operation_result_t update(key_t key, value_spanc_t value) override;
        operation_result_t remove(key_t key) override;
        operation_result_t read(key_t key, value_span_t value) const override;

        operation_result_t batch_upsert(keys_spanc_t keys, values_spanc_t values, value_lengths_spanc_t sizes) override;
        operation_result_t batch_read(keys_spanc_t keys, values_span_t values) const override;

        operation_result_t bulk_load(keys_spanc_t keys, values_spanc_t values, value_lengths_spanc_t sizes) override;

        operation_result_t range_select(key_t key, size_t length, values_span_t values) const override;
        operation_result_t scan(key_t key, size_t length, value_span_t single_value) const override;

        void flush() override;

        size_t size_on_disk() const override;

        std::unique_ptr<transaction_t> create_transaction() override;

    private:
        std::unique_ptr<std::unordered_map<key_t, value_spanc_t>> map_;
        mutable std::mutex map_lock_;
        fs::path save_path;
    };

    inline void plainhash_t::set_config(fs::path const& config_path, fs::path const& main_dir_path,
                                        std::vector<fs::path> const& storage_dir_paths,
                                        [[maybe_unused]] db_hints_t const& hints)
    {
        save_path = main_dir_path / "data.json";
    }

    inline bool plainhash_t::open(std::string& error)
    {
        if (!fs::exists(save_path))
        {
            return true;
        }

        std::ifstream data_json(save_path);
        nlohmann::json read_json;
        data_json >> read_json;

        for (auto& el : read_json.items())
        {
            auto value = el.value().get<std::string>();
            map_->insert({atoi(el.key().c_str()), decodeBase64(value)});
        }
        return true;
    }

    void plainhash_t::save_as_json() const
    {
        nlohmann::json out_json;

        for (const auto& [key, value] : *map_)
        {
            size_t out_len = 4 * ((value.size() + 2) / 3);
            std::vector<char> encoded(out_len);

            base64_encode(reinterpret_cast<const char*>(value.data()), value.size(), encoded.data(), &out_len, 0);
            out_json[std::to_string(key)] = std::string(encoded.data(), out_len);
        }


        if (!fs::exists(save_path.parent_path()))
        {
            std::cerr << "Directory does not exist: " << save_path.parent_path() << std::endl;
        }
        std::ofstream data_json(save_path);
        if (!data_json.is_open())
        {
            std::cerr << "Failed to open file: " << save_path << std::endl;
        }
        data_json << out_json.dump(2);
        data_json.flush();
        data_json.close();
    }

    inline void plainhash_t::close()
    {
        // save_as_json();
    }

    inline operation_result_t plainhash_t::upsert(key_t key, value_spanc_t value)
    {
        map_lock_.lock();
        map_->insert({key, std::move(std::vector(value.begin(), value.end()))});
        map_lock_.unlock();
        return {1, operation_status_t::ok_k};
    }

    inline operation_result_t plainhash_t::update(key_t key, value_spanc_t value)
    {
        map_lock_.lock();
        if (map_->contains(key))
        {
            map_->insert({key, std::move(std::vector(value.begin(), value.end()))});
            map_lock_.unlock();
            return {1, operation_status_t::ok_k};
        }
        else
        {
            map_lock_.unlock();
            return {0, operation_status_t::not_found_k};
        }
    }

    inline operation_result_t plainhash_t::remove(key_t key)
    {
        map_lock_.lock();
        if (map_->contains(key))
        {
            map_->erase(key);
            map_lock_.unlock();
            return {1, operation_status_t::ok_k};
        }
        else
        {
            map_lock_.unlock();
            return {0, operation_status_t::not_found_k};
        }
    }

    inline operation_result_t plainhash_t::read(key_t key, value_span_t value) const
    {
        map_lock_.lock();
        if (map_->contains(key))
        {
            auto data = map_->at(key);
            memcpy(value.data(), data.data(), data.size());
            map_lock_.unlock();
            return {1, operation_status_t::ok_k};
        }
        else
        {
            map_lock_.unlock();
            return {0, operation_status_t::not_found_k};
        }
    }

    inline operation_result_t plainhash_t::batch_upsert(keys_spanc_t keys, values_spanc_t values,
                                                        value_lengths_spanc_t sizes)
    {
        map_lock_.lock();
        size_t offset = 0;
        for (size_t idx = 0; idx < keys.size(); ++idx)
        {
            key_t key = keys[idx];
            auto value = values.subspan(offset, sizes[idx]);
            map_->insert({key, std::move(std::vector(value.begin(), value.end()))});
            offset += sizes[idx];
        }
        map_lock_.unlock();
        return {keys.size(), operation_status_t::ok_k};
    }

    inline operation_result_t plainhash_t::batch_read(keys_spanc_t keys, values_span_t values) const
    {
        map_lock_.lock();
        // Note: imitation of batch read!
        size_t offset = 0;
        for (auto key : keys)
        {
            auto data = map_->at(key);
            memcpy(values.data() + offset, data.data(), data.size());
            offset += data.size();
        }
        map_lock_.unlock();
        return {keys.size(), operation_status_t::ok_k};
    }

    inline operation_result_t plainhash_t::bulk_load(keys_spanc_t keys, values_spanc_t values,
                                                     value_lengths_spanc_t sizes)
    {
        return batch_upsert(keys, values, sizes);
    }

    inline operation_result_t plainhash_t::range_select(key_t key, size_t length, values_span_t values) const
    {
        return {0, operation_status_t::not_implemented_k};
    }

    inline operation_result_t plainhash_t::scan(key_t key, size_t length, value_span_t single_value) const
    {
        return {0, operation_status_t::not_implemented_k};
    }

    inline std::string plainhash_t::info() { return fmt::format(""); }

    inline void plainhash_t::flush()
    {
        // Nothing to do
    }

    inline size_t plainhash_t::size_on_disk() const
    {
        if (fs::exists(save_path))
        {
            return fs::file_size(save_path);
        }
        else
        {
            return 0;
        }
    }

    std::unique_ptr<transaction_t> plainhash_t::create_transaction() { return {}; }


} // namespace ucsb::plain
