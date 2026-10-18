#include "audio/pcm_converter.hpp"
#include "transport/radio.hpp"
#include "transport/windows.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
struct RadioState { DWORD status; bool connected; };
struct RadioScenario {
    std::vector<RadioState> radios;
    std::size_t cursor=0,queries=0,closed=0,searches_closed=0;
    bool fail_next=false;
} scenario;
constexpr std::uint64_t headphone_address=0x123456789abc;
void require_radio(bool condition) {
    if (!condition) throw std::runtime_error("Bluetooth radio selection or handle lifetime failed");
}
HBLUETOOTH_RADIO_FIND WINAPI first_radio(const BLUETOOTH_FIND_RADIO_PARAMS*,HANDLE* radio) {
    if (scenario.radios.empty()) { SetLastError(ERROR_NO_MORE_ITEMS); return nullptr; }
    *radio=reinterpret_cast<HANDLE>(1);
    return reinterpret_cast<HBLUETOOTH_RADIO_FIND>(100);
}
BOOL WINAPI next_radio(HBLUETOOTH_RADIO_FIND,HANDLE* radio) {
    if (scenario.fail_next) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (++scenario.cursor==scenario.radios.size()) { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
    *radio=reinterpret_cast<HANDLE>(scenario.cursor+1);
    return TRUE;
}
BOOL WINAPI finish_radios(HBLUETOOTH_RADIO_FIND) { ++scenario.searches_closed; return TRUE; }
BOOL WINAPI close_radio(HANDLE) { ++scenario.closed; return TRUE; }
DWORD WINAPI device_info(HANDLE radio,BLUETOOTH_DEVICE_INFO* info) {
    require_radio(info->dwSize==sizeof(*info) && info->Address.ullLong==headphone_address);
    const auto index=reinterpret_cast<std::uintptr_t>(radio)-1;
    ++scenario.queries;
    const auto state=scenario.radios.at(index);
    if (state.status==ERROR_SUCCESS) {
        info->fConnected=state.connected;
        info->fRemembered=TRUE;
        info->szName[0]=static_cast<wchar_t>(L'1'+index);
    }
    return state.status;
}
void check_radio_selection() {
    const lhdc::detail::RadioApi api{first_radio,next_radio,finish_radios,device_info,close_radio};
    auto query=[&](std::vector<RadioState> radios) {
        scenario={std::move(radios)};
        const auto result=lhdc::detail::query_connection(headphone_address,api);
        require_radio(scenario.closed==scenario.queries);
        require_radio(scenario.searches_closed==(scenario.radios.empty()?0u:1u));
        return result;
    };
    const auto absent=query({});
    require_radio(!absent.radio && !absent.known && !absent.device.fConnected);
    const auto unknown=query({{ERROR_NOT_FOUND,false},{ERROR_NOT_FOUND,false}});
    require_radio(unknown.radio && !unknown.known && scenario.queries==2);
    const auto second=query({{ERROR_NOT_FOUND,false},{ERROR_SUCCESS,true}});
    require_radio(second.known && second.device.fConnected && second.device.szName[0]==L'2');
    const auto stale=query({{ERROR_SUCCESS,false},{ERROR_SUCCESS,true}});
    require_radio(stale.device.fConnected && stale.device.szName[0]==L'2');
    const auto disconnected=query({{ERROR_SUCCESS,false},{ERROR_NOT_FOUND,false}});
    require_radio(disconnected.known && !disconnected.device.fConnected && disconnected.device.szName[0]==L'1');
    const auto first=query({{ERROR_SUCCESS,true},{ERROR_SUCCESS,false}});
    require_radio(first.device.fConnected && scenario.queries==1);
    for (const auto fail_next:{false,true}) {
        scenario={{{static_cast<DWORD>(fail_next?ERROR_NOT_FOUND:ERROR_ACCESS_DENIED),false}}};
        scenario.fail_next=fail_next;
        bool rejected=false;
        try { lhdc::detail::query_connection(headphone_address,api); }
        catch (const lhdc::WindowsError& error) { rejected=error.code==ERROR_ACCESS_DENIED; }
        require_radio(rejected && scenario.closed==1 && scenario.searches_closed==1);
    }
}
}
int main() {
    try {
        check_radio_selection();
        bool rejected=false;
        try { lhdc::PcmConverter wrong_rate(44100,24,false,{48000,24,400}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected) throw std::runtime_error("Cross-rate PCM must be rejected");
        constexpr double pi=3.14159265358979323846;
        unsigned checked=0;
        for (auto rate:{44100u,48000u,96000u,192000u}) for (auto bits:{16u,24u}) {
            lhdc::PcmConverter converter(rate,bits,false,{rate,bits,400});
            std::vector<std::uint8_t> input(960*(bits/8));
            for (std::size_t i=0;i<input.size();++i) input[i]=static_cast<std::uint8_t>(i*37);
            if (converter.convert(input)!=input) throw std::runtime_error("Native-rate integer PCM must remain bit-exact");
        }
        for (auto rate:{44100u,48000u,96000u,192000u}) {
            lhdc::PcmConverter converter(rate,32,true,{rate,24,400});
            const float samples[]={-2.0f,-1.0f,-0.5f,0.0f,0.5f,1.0f,2.0f,0.0f};
            const std::int32_t expected[]={-8388608,-8388608,-4194304,0,4194304,8388607,8388607,0};
            const auto input=std::span(reinterpret_cast<const std::uint8_t*>(samples),sizeof(samples));
            const auto output=converter.convert(input);
            if (output.size()!=24) throw std::runtime_error("Same-rate float conversion changed the sample count");
            for (unsigned i=0;i<8;++i) {
                std::int32_t value=output[i*3]|(output[i*3+1]<<8)|(output[i*3+2]<<16);
                if (value&0x800000) value-=0x1000000;
                if (value!=expected[i]) throw std::runtime_error("Float-to-24-bit clipping or scaling changed");
            }
        }
        for (auto input_rate:{44100u,48000u}) for (auto input_bits:{16u,24u,32u})
        for (auto bits:{16u,24u}) {
            const auto rate=input_rate;
            lhdc::PcmConverter converter(input_rate,input_bits,input_bits==32,{rate,bits,rate==44100?240u:256u});
            std::vector<std::uint8_t> output;
            unsigned position=0;
            for (unsigned i=0;i<100;++i) {
                const auto frames=input_rate/100;
                std::vector<std::uint8_t> data(frames*2*(input_bits/8));
                for (unsigned frame=0;frame<frames;++frame) for (unsigned channel=0;channel<2;++channel) {
                    const auto value=0.1*std::sin(2*pi*(channel?880:440)*(position+frame)/input_rate);
                    auto* sample=data.data()+(frame*2+channel)*(input_bits/8);
                    if (input_bits==32) { const float fp=static_cast<float>(value); std::memcpy(sample,&fp,4); }
                    else {
                        const auto pcm=static_cast<std::int32_t>(std::lround(value*(input_bits==16?32768:8388608)));
                        for (unsigned byte=0;byte<input_bits/8;++byte) sample[byte]=static_cast<std::uint8_t>(static_cast<std::uint32_t>(pcm)>>(byte*8));
                    }
                }
                position+=frames;
                auto block=converter.convert(data); output.insert(output.end(),block.begin(),block.end());
            }
            const auto frames=output.size()/(2*(bits/8));
            if (frames!=rate) throw std::runtime_error("Format conversion changed PCM duration");
            for (unsigned channel=0;channel<2;++channel) {
                double energy=0,real=0,imag=0,peak=0;
                const auto frequency=channel?880:440;
                for (std::size_t i=256;i<frames;++i) {
                    const auto* data=output.data()+(i*2+channel)*(bits/8);
                    std::int32_t sample=static_cast<std::int16_t>(data[0]|(data[1]<<8));
                    if (bits==24) { sample=data[0]|(data[1]<<8)|(data[2]<<16); if (sample&0x800000) sample-=0x1000000; }
                    const double value=sample/(bits==16?32768.0:8388608.0);
                    const double angle=2*pi*frequency*i/rate;
                    real+=value*std::cos(angle); imag+=value*std::sin(angle);
                    energy+=value*value; peak=std::max(peak,std::abs(value));
                }
                const auto count=frames-256;
                const auto amplitude=2*std::hypot(real,imag)/count;
                if (peak<0.099 || peak>0.102 || amplitude<0.098 || amplitude>0.102 || energy/count<0.0048)
                    throw std::runtime_error("Format conversion changed channel frequency or level");
            }
            ++checked;
        }
        std::cout << checked << " PCM representation conversions preserve duration, stereo frequency and level\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
