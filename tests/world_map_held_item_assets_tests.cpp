#include "awl/world_map_held_item_assets.h"
#include "awl/filesystem.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

namespace allocation_probe { bool enabled=false; size_t remaining=0,live=0; }
void* operator new(size_t size) {
    if(allocation_probe::enabled && allocation_probe::remaining--==0)throw std::bad_alloc();
    if(void* p=std::malloc(size?size:1)){++allocation_probe::live;return p;}throw std::bad_alloc();
}
void operator delete(void* p) noexcept {if(p){--allocation_probe::live;std::free(p);}}
void* operator new[](size_t s){return ::operator new(s);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,size_t) noexcept {::operator delete(p);}

namespace {
using Status=awl::WorldMapHeldItemAssetsStatus;
using Bank=awl::WorldMapHeldItemFeatureBank;
using FeatureStatus=awl::WorldMapModelFeatureStatus;
int failures=0;
void expect(bool yes,const char* message){if(!yes){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
void word(std::vector<uint8_t>& b,size_t p,uint32_t value){for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(value>>(24-i*8));}
std::vector<uint8_t> gpl(){
    std::vector<uint8_t> b(0x160);word(b,0,0x005bbc61);word(b,12,2);word(b,16,20);
    word(b,20,0x40);word(b,24,0x140);word(b,28,0xc0);word(b,32,0x146);
    for(size_t p:{size_t(0x40),size_t(0xc0)}){
        word(b,p+16,20);word(b,p+24,32);b[p+28]=0;b[p+29]=3;
        b[p+32]=1;b[p+33]=0;b[p+48]=3;b[p+49]=0;b[p+64]=1;b[p+65]=1;
    }
    for(size_t i=0;i<6;++i)b[0x140+i]=uint8_t("alpha"[i]);
    for(size_t i=0;i<5;++i)b[0x146+i]=uint8_t("beta"[i]);return b;
}
std::vector<uint8_t> tpl(){
    std::vector<uint8_t> b(192);word(b,0,0x0020af30);word(b,4,2);word(b,8,12);
    word(b,12,32);word(b,20,68);
    for(size_t i=0;i<2;++i){const size_t p=32+i*36;b[p+1]=4;b[p+3]=4;word(b,p+4,14);word(b,p+8,uint32_t(128+i*32));}
    return b;
}
struct Fixture {
    std::filesystem::path root;
    Fixture(){
        root=std::filesystem::temp_directory_path()/std::filesystem::path("awl-held-assets-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if(!std::filesystem::create_directory(root))throw std::runtime_error("Fixture directory already exists");
        std::filesystem::create_directory(root/"files");
    }
    ~Fixture(){
        std::error_code error;
        for(const char* name:{"symbol.gpl","symbol.tpl","real.gpl","real.tpl"})std::filesystem::remove(root/"files"/name,error);
        std::filesystem::remove(root/"files",error);std::filesystem::remove(root,error);
    }
    void write(const char* name,const std::vector<uint8_t>& bytes){std::ofstream f(root/"files"/name,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));if(!f)throw std::runtime_error("Fixture write failed");}
    void complete(){write("symbol.gpl",gpl());write("symbol.tpl",tpl());write("real.gpl",gpl());write("real.tpl",tpl());}
    void mount(){expect(awl::filesystem_mount("/",root.string().c_str()),"synthetic mounted directory is accepted");}
};
void metadata_tests(){
    const auto b=gpl();awl::WorldMapHeldItemGplMetadata out;
    expect(awl::prepare_world_map_held_item_gpl_metadata(b,&out)==Status::Prepared && out.sections.size()==2 &&
        out.sections[0].offset==0x40 && out.sections[1].offset==0xc0 && out.sections[0].name_offset==0x140 &&
        out.sections[1].material_offset==0xd4 && out.sections[0].commands.size()==3 && out.sections[0].commands[2].channel_1==1,
        "paired table preserves full section order, material offsets and serialized command headers");
    for(size_t length=0;length<0x140;++length){auto truncated=b;truncated.resize(length);
        expect(awl::prepare_world_map_held_item_gpl_metadata(truncated,&out)!=Status::Prepared && out.sections.size()==2,"all truncated required extents reject without replacing metadata");}
    for(unsigned fault=0;fault<14;++fault){auto bad=b;Status expected=Status::InvalidInput;
        if(fault==0){word(bad,0,0xffffffff);expected=Status::UnsupportedLayout;}
        if(fault==1){word(bad,4,1);expected=Status::UnsupportedLayout;}
        if(fault==2)word(bad,12,65536);
        if(fault==3)word(bad,16,0xfffffffcu);
        if(fault==4)word(bad,20,21);
        if(fault==5){word(bad,28,0x40);expected=Status::UnsupportedLayout;}
        if(fault==6)word(bad,24,uint32_t(bad.size()));
        if(fault==7){word(bad,0x50,0);expected=Status::UnsupportedLayout;}
        if(fault==8)word(bad,0x58,0x7c);
        if(fault==9){word(bad,0x58,20);expected=Status::UnsupportedLayout;}
        if(fault==10){bad[0x60]=0x80;expected=Status::UnsupportedLayout;}
        if(fault==11){word(bad,0x40,0x60);word(bad,0xa0,0x70);bad[0xa7]=6;expected=Status::RequiresSkin;}
        if(fault==12){word(bad,0x40,32);expected=Status::UnsupportedLayout;}
        if(fault==13){word(bad,0x40,20);expected=Status::UnsupportedLayout;}
        expect(awl::prepare_world_map_held_item_gpl_metadata(bad,&out)==expected && out.sections.size()==2 && out.sections[1].offset==0xc0,
            "invalid/unsupported layout and reached skin requirement retain metadata output");
    }
    auto zero=b;zero[0x5c]=zero[0x5d]=0;word(zero,0x58,0);
    expect(awl::prepare_world_map_held_item_gpl_metadata(zero,&out)==Status::Prepared && out.sections[0].commands.empty(),"zero count does not require a command payload");
    auto no_skin=b;word(no_skin,0x40,0x60);no_skin[0xa7]=6;
    expect(awl::prepare_world_map_held_item_gpl_metadata(no_skin,&out)==Status::Prepared,"component count six with null position data skips SKN loading");
    expect(awl::prepare_world_map_held_item_gpl_metadata(b,nullptr)==Status::InvalidInput,"null metadata output rejects");
}
void owned_tests(){
    Fixture f;f.mount();std::shared_ptr<const awl::WorldMapHeldItemAssets> assets;
    expect(awl::load_world_map_held_item_assets(&assets)==Status::ReadFailure && !assets,"missing first file leaves no partial owner");
    f.write("symbol.gpl",gpl());f.write("symbol.tpl",tpl());f.write("real.gpl",gpl());
    expect(awl::load_world_map_held_item_assets(&assets)==Status::TextureFailure && !assets,"missing final texture file discards both pending banks");
    f.write("real.tpl",std::vector<uint8_t>(12));
    expect(awl::load_world_map_held_item_assets(&assets)==Status::TextureFailure && !assets,"malformed final texture bank is rejected");
    f.complete();expect(awl::load_world_map_held_item_assets(&assets)==Status::Loaded && assets,"all four synthetic files produce an owned bundle");if(!assets)return;
    const auto* owner=assets.get();f.write("symbol.gpl",{});
    expect(awl::load_world_map_held_item_assets(&assets)==Status::Loaded && assets.get()==owner,"already loaded bundle performs no new file reads");f.complete();
    awl::WorldMapHeldItemFeatureObservations o;o.row=awl::WorldMapHeldItemFeatureRow{7,3,1,0,0,1,0,1};
    expect(assets->resolve({Bank::Primary,1,{0,1}},&o) && o.resource && o.resource->commands->size()==3 && o.row->item_id==7,
        "selected bounds produce complete borrowed observations while preserving supplied row");
    const auto key=o.resource->resource_0;
    expect(!assets->resolve({Bank::Primary,2,{0,1}},&o) && !assets->resolve({Bank::Primary,1,{0,2}},&o) &&
        !assets->resolve({static_cast<Bank>(99),1,{0,1}},&o) && o.resource->resource_0==key,"invalid group/texture/bank preserves observations");
    expect(assets->texture(Bank::Primary,1) && assets->texture(Bank::Primary,1)->mip_levels.size()==1 &&
        !assets->texture(Bank::Primary,2) && !assets->texture(static_cast<Bank>(99),0),"owned texture views retain bounded complete encoded mip metadata");
    std::unique_ptr<awl::WorldMapNativeModelFeature> feature;awl::WorldMapHeldItemFeatureStep step;
    expect(awl::construct_world_map_native_model_feature(2,&feature)==FeatureStatus::Constructed,"native feature constructs");if(!feature)return;
    auto row=*o.row;
    expect(awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),7,row,std::nullopt,assets,&step)==FeatureStatus::Advanced &&
        feature->state().cache_48[0].texture->index==0 && feature->state().cache_48[1].texture->index==1 && feature->attachment_source(),"owned files drive the complete native feature update");
    std::weak_ptr<const awl::WorldMapHeldItemAssets> prior=assets;assets.reset();
    expect(!prior.expired() && awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),0,std::nullopt,std::nullopt,nullptr,&step)==FeatureStatus::Advanced &&
        !feature->attachment_source() && feature->state().cache_48[1].texture->index==1,"feature retains its asset owner through external release and empty reset");
    auto alternate=gpl();alternate[0xc0+64]=3;f.write("real.gpl",alternate);
    expect(awl::load_world_map_held_item_assets(&assets)==Status::Loaded,"a fresh supplied bank constructs while earlier cache bank remains live");
    const auto retained_texture_bank=feature->state().cache_48[1].texture->bank_identity;
    row.type_0=1;row.alternate_group_3=1;
    expect(awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),7,row,uint8_t{0},assets,&step)==FeatureStatus::Advanced &&
        step.route->bank==Bank::Alternate && feature->state().cache_48[1].texture->bank_identity==retained_texture_bank && !prior.expired(),
        "alternate update without channel one retains its earlier texture key and asset provider");
    expect(awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),7,row,uint8_t{0},assets,&step)==FeatureStatus::Advanced &&
        assets.use_count()==2,"repeated use deduplicates the same retained bank owner");
    const auto resource=feature->state().resource_0;
    row.alternate_texture_a=2;
    expect(awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),7,row,uint8_t{0},assets,&step)==FeatureStatus::InvalidInput &&
        feature->state().resource_0==resource && step.after.resource_0==resource,"invalid reached texture cannot publish a feature prefix");
    expect(awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),7,std::nullopt,std::nullopt,nullptr,&step)==FeatureStatus::RequiresRow &&
        awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),7,row,std::nullopt,nullptr,&step)==FeatureStatus::RequiresBaseline &&
        awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),7,row,uint8_t{0},nullptr,&step)==FeatureStatus::RequiresResource &&
        feature->state().resource_0==resource,"missing reached evidence follows route order and preserves the owner");
    feature.reset();expect(prior.expired(),"feature destruction releases conservatively retained earlier asset providers");
    auto missing_image=tpl();word(missing_image,20,0);f.write("real.tpl",missing_image);
    std::shared_ptr<const awl::WorldMapHeldItemAssets> unsafe;
    expect(awl::load_world_map_held_item_assets(&unsafe)==Status::Loaded && !unsafe->resolve({Bank::Alternate,1,{0,1}},&o),
        "null selected image descriptor is rejected without interpreting it as a usable texture");
    unsafe.reset();auto unpacked=tpl();unpacked[68+35]=1;f.write("real.tpl",unpacked);
    expect(awl::load_world_map_held_item_assets(&unsafe)==Status::Loaded && !unsafe->resolve({Bank::Alternate,1,{0,1}},&o),
        "selected serialized unpacked image is unsupported rather than treated as a relocated native pointer");
    unsafe.reset();f.write("real.tpl",tpl());
#if !defined(_MSC_VER) || !defined(_DEBUG)
    std::shared_ptr<const awl::WorldMapHeldItemAssets> pending;size_t rejected=0;bool loaded=false;
    const auto live=allocation_probe::live;
    for(size_t fail=0;fail<512;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::load_world_map_held_item_assets(&pending);allocation_probe::enabled=false;
        if(status==Status::Loaded){loaded=true;break;}++rejected;
        expect((status==Status::AllocationFailure || status==Status::TextureFailure) && !pending && allocation_probe::live==live,
            "each caught loader allocation failure releases pending banks and publishes nothing");
    }
    expect(loaded && rejected>10,"loader allocation sweep reaches completion");std::cout<<"HELD_ASSET_LOAD_ALLOCATION_FAILURES "<<rejected<<'\n';
    expect(awl::construct_world_map_native_model_feature(2,&feature)==FeatureStatus::Constructed,"failure-sweep feature constructs");row.alternate_texture_a=1;bool advanced=false;rejected=0;
    step.after.resource_0=123;const auto update_live=allocation_probe::live;
    for(size_t fail=0;fail<64;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),7,row,uint8_t{0},assets,&step);allocation_probe::enabled=false;
        if(status==FeatureStatus::Advanced){advanced=true;break;}++rejected;
        expect(status==FeatureStatus::AllocationFailure && allocation_probe::live==update_live && feature->state().resource_0==0 &&
            step.after.resource_0==123 && assets.use_count()==1,"every bank-binding allocation failure retains feature/output/provider lifetime");
    }
    expect(advanced && rejected>10,"binding allocation sweep reaches provider publication");std::cout<<"HELD_ASSET_BIND_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}
void hash(uint64_t& h,uint32_t w){for(unsigned i=0;i<4;++i){h^=(w>>(24-i*8))&255;h*=1099511628211ull;}}
void local(const char* disc){
    expect(awl::filesystem_mount("/",disc),"local mounted disc is accepted");std::shared_ptr<const awl::WorldMapHeldItemAssets> assets;
    expect(awl::load_world_map_held_item_assets(&assets)==Status::Loaded,"local symbol/real GPL and TPL files load");if(!assets)return;
    uint64_t digest=14695981039346656037ull;size_t sections=0,commands=0;
    std::unique_ptr<awl::WorldMapNativeModelFeature> feature;expect(awl::construct_world_map_native_model_feature(2,&feature)==FeatureStatus::Constructed,"local feature constructs");
    if(!feature)return;awl::WorldMapHeldItemFeatureStep step;
    for(Bank bank:{Bank::Primary,Bank::Alternate}){
        const auto& metadata=assets->metadata(bank);hash(digest,uint32_t(metadata.sections.size()));hash(digest,uint32_t(assets->texture_count(bank)));
        for(size_t i=0;i<metadata.sections.size();++i){const auto& s=metadata.sections[i];++sections;commands+=s.commands.size();
            for(uint32_t w:{s.offset,s.name_offset,s.material_offset,uint32_t(s.commands.size())})hash(digest,w);
            for(const auto& c:s.commands){hash(digest,c.type_0);hash(digest,c.channel_1);}
            awl::WorldMapHeldItemFeatureRow row{7,uint8_t(bank==Bank::Primary?3:1),uint8_t(i),uint8_t(i),0,0,0,0};
            expect(awl::advance_world_map_native_held_item_feature_from_assets(feature.get(),7,row,uint8_t{255},assets,&step)==FeatureStatus::Advanced &&
                step.route && step.route->bank==bank && step.route->group==i && feature->attachment_source(),"every supplied local group updates the native feature against owned asset metadata");
        }
        for(size_t i=0;i<assets->texture_count(bank);++i){const auto* texture=assets->texture(bank,uint16_t(i));
            for(uint32_t w:{texture->image_header_offset,texture->palette_header_offset,uint32_t(texture->header.height),
                uint32_t(texture->header.width),texture->header.format,texture->header.data_offset})hash(digest,w);
        }
    }
    std::cout<<"LOCAL_HELD_ASSET_METADATA "<<sections<<' '<<commands<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(sections==223 && commands==676 && digest==0xc2e085b94da8285eull,
        "local section/command references and all texture descriptors match independent mapped metadata digest");
}
} // namespace
int main(int argc,char** argv){
    metadata_tests();owned_tests();awl::filesystem_shutdown();
    if(argc==3 && std::string(argv[1])=="--held-assets-local")local(argv[2]);
    else if(argc!=1)expect(false,"usage: --held-assets-local <disc>");
    awl::filesystem_shutdown();return failures?1:0;
}
