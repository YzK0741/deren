#pragma once
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <glm/glm.hpp>

namespace deren::app::radiance_capture {
    struct options {
        bool enabled=false,touched=false,invalid=false;
        std::filesystem::path output;
        glm::vec3 position{0};
        int size=256;
        float near_plane=.05f,far_plane=1000;
    };
    inline std::optional<float> number(std::string_view text) {
        if(text.empty())return {};
        std::string copy(text);char* end=nullptr;float v=std::strtof(copy.c_str(),&end);
        if(end!=copy.c_str()+copy.size() || !std::isfinite(v))return {};
        return v;
    }
    inline std::optional<glm::vec3> parse_position(std::string_view text) {
        glm::vec3 position{};
        for(unsigned i=0;i<3;++i) {
            auto comma=text.find(',');
            if((i<2 && comma==text.npos) || (i==2 && comma!=text.npos))return {};
            auto value=number(text.substr(0,comma));if(!value)return {};
            position[i]=*value;
            if(i<2)text.remove_prefix(comma+1);
        }
        return position;
    }
    inline bool valid(options const& o) noexcept {
        if(o.invalid)return false;
        if(!o.enabled)return !o.touched;
        if(o.output.empty() || o.size<16 || o.size>512 || (o.size&(o.size-1)))return false;
        if(!std::isfinite(o.near_plane)||!std::isfinite(o.far_plane)||o.near_plane<=0||o.far_plane<=o.near_plane||o.far_plane>1000000)return false;
        for(unsigned i=0;i<3;++i)if(!std::isfinite(o.position[i])||std::abs(o.position[i])>1000000)return false;
        return true;
    }
    inline bool parse_option(options& o, std::string_view key, std::string_view value) {
        if (key == "--capture-radiance-cube") { o.enabled = true; o.output = value; return true; }
        if (key == "--probe-position") { o.touched = true; auto p = parse_position(value); if (p) o.position = *p; else o.invalid = true; return true; }
        if (key != "--probe-size" && key != "--probe-near" && key != "--probe-far") return false;
        o.touched = true;
        auto n = number(value);
        if (!n) { o.invalid = true; return true; }
        if (key == "--probe-size") {
            if (*n < 16 || *n > 512 || std::floor(*n) != *n) o.invalid = true;
            else o.size = static_cast<int>(*n);
        } else if (key == "--probe-near") o.near_plane = *n;
        else o.far_plane = *n;
        return true;
    }
    enum class write_error {invalid_pixels,exists,open,write,rename};
    inline char const* describe(write_error error) {
        switch(error) {
        case write_error::invalid_pixels:return "invalid cube dimensions or radiance";
        case write_error::exists:return "output already exists";
        case write_error::open:return "could not open output temporary file";
        case write_error::write:return "could not finish output write";
        default:return "could not publish output file";
        }
    }
    inline std::expected<void,write_error> write_cube(std::filesystem::path const& path,int size,std::span<float const> pixels) {
        if constexpr(std::endian::native!=std::endian::little)return std::unexpected(write_error::invalid_pixels);
        if(size<16||size>512||(size&(size-1))||pixels.size()!=std::size_t(6)*size*size*4)return std::unexpected(write_error::invalid_pixels);
        for(std::size_t i=0;i<pixels.size();++i)if(!std::isfinite(pixels[i])||pixels[i]<0||pixels[i]>65504||(i%4==3&&pixels[i]!=1))return std::unexpected(write_error::invalid_pixels);
        std::error_code ec;
        if(std::filesystem::exists(path,ec)||ec)return std::unexpected(write_error::exists);
        auto temporary=path;
        temporary += ".part-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ofstream out(temporary,std::ios::binary);
        if(!out)return std::unexpected(write_error::open);
        std::uint32_t width=static_cast<std::uint32_t>(size),mips=1;
        out.write("IBL1",4);out.write(reinterpret_cast<char*>(&width),4);out.write(reinterpret_cast<char*>(&mips),4);
        out.write(reinterpret_cast<char const*>(pixels.data()),static_cast<std::streamsize>(pixels.size_bytes()));
        out.close();
        if(!out) {std::filesystem::remove(temporary,ec);return std::unexpected(write_error::write);}
        std::filesystem::rename(temporary,path,ec);
        if(ec) {std::filesystem::remove(temporary,ec);return std::unexpected(write_error::rename);}
        return {};
    }
}
