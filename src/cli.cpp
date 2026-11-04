#include "nbb/core.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc==2 && std::wstring(argv[1])==L"--fields") {std::cout<<nbb::Json(nbb::fieldNames).dump(2)<<'\n';return 0;}
        if(argc!=4) { std::cerr<<"Usage: nbb_cli manifest.json config.json report.json\n"; return 2; }
        auto configPath=std::filesystem::path(argv[2]);
        auto exclusions=nbb::readExclusions(configPath.parent_path()/"NPCBehaviorBridge.exclude.txt");
        auto result=nbb::run(nbb::readJson(std::filesystem::path(argv[1])),nbb::readJson(configPath),exclusions);
        nbb::writeJson(std::filesystem::path(argv[3]),result);
        std::cout<<result.at("summary").dump(2)<<'\n'; return 0;
    } catch(const std::exception& e) { std::cerr<<"No plan produced: "<<e.what()<<'\n'; return 1; }
}
