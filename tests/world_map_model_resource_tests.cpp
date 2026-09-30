#include "awl/world_map_secondary_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace {
using Status = awl::WorldMapModelPreparationStatus;
using Matrix = awl::WorldMapModelMatrix;
using Pose = std::array<uint32_t, 13>;
int failures = 0;
void expect(bool yes, const char* message) { if (!yes) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
uint32_t bits(float value) { uint32_t word; std::memcpy(&word,&value,4); return word; }
float value(uint32_t word) { float result; std::memcpy(&result,&word,4); return result; }
void put(std::vector<uint8_t>& b, size_t offset, uint32_t word) {
    for (unsigned i=0;i<4;++i) b[offset+i]=static_cast<uint8_t>(word>>(24-i*8));
}
bool close(const Matrix& a,const Matrix& b) {
    for (size_t i=0;i<a.size();++i) if (!std::isfinite(a[i]) || !std::isfinite(b[i]) ||
        std::abs(a[i]-b[i])>2e-6f*std::max(1.0f,std::abs(b[i]))) return false;
    return true;
}
Pose pose(uint32_t flags=13) {
    // Invented non-unit quaternion: 180 degrees around X, then unequal
    // signed column scales and translation. Unread fields are opaque.
    return {flags<<24,bits(2),bits(-3),bits(4),bits(2),bits(0),bits(0),bits(0),
        bits(7),bits(8),bits(9),0x12345678,0xabcdef01};
}
std::vector<uint8_t> archive() {
    // One invented flat-U8 ACT file, two 0x1C records, disjoint 0x34 poses.
    std::vector<uint8_t> b(64+256);
    put(b,0,0x55aa382d);put(b,4,32);put(b,8,32);put(b,12,64);
    put(b,32,0x01000000);put(b,40,2);put(b,44,1);put(b,48,64);put(b,52,256);b[57]='p';
    const size_t base=64;put(b,base,0x007b7960);put(b,base+4,2);put(b,base+12,32);put(b,base+16,240);
    put(b,base+24,5);put(b,base+28,244);
    for (unsigned i=0;i<2;++i) {
        const size_t record=base+32+i*28, offset=96+i*52;
        put(b,record,static_cast<uint32_t>(offset));put(b,record+4,240);put(b,record+8,0);
        put(b,record+12,32);put(b,record+16,255);put(b,record+20,0xfeedbeef);put(b,record+24,0x87654321);
        auto p=pose(i==0?13u:0u);
        for(size_t j=0;j<p.size();++j) put(b,base+offset+j*4,p[j]);
    }
    return b;
}
void test_pose() {
    Matrix result{};
    expect(awl::prepare_world_map_model_pose(pose(),&result)==Status::Prepared &&
        close(result,{2,0,0,7, 0,3,0,8, 0,0,-4,9}),"non-unit quaternion, signed column scaling and translation compose in original order");
    auto p=pose(31);for(size_t i=1;i<p.size();++i)p[i]=bits(static_cast<float>(i)+0.25f);
    Matrix expected{};for(size_t i=0;i<12;++i)expected[i]=value(p[i+1]);
    expect(awl::prepare_world_map_model_pose(p,&result)==Status::Prepared && result==expected,
        "explicit matrix overrides all quaternion/Euler/scale/translation flags");
    p=pose(0xe0);for(size_t i=1;i<p.size();++i)p[i]=0x7fc00000;
    expect(awl::prepare_world_map_model_pose(p,&result)==Status::Prepared && close(result,{1,0,0,0,0,1,0,0,0,0,1,0}),
        "unreached pose fields and unrelated flag bits stay opaque");
    p=pose(11);p[4]=bits(-0.0f);p[5]=p[6]=0;
    expect(awl::prepare_world_map_model_pose(p,&result)==Status::Prepared && close(result,{2,0,0,7,0,-3,0,8,0,0,4,9}),
        "zero Euler rotations take identity then scale/translation");
    const auto saved=result;p[4]=bits(90);
    expect(awl::prepare_world_map_model_pose(p,&result)==Status::RequiresEulerRotation && result==saved,
        "nonzero Euler requires translation before any output acceptance");
    p=pose(6);p[4]=0;p[5]=0;p[6]=0;p[7]=bits(2);
    expect(awl::prepare_world_map_model_pose(p,&result)==Status::Prepared && close(result,{1,0,0,0,0,1,0,0,0,0,1,0}),
        "quaternion flag takes priority over Euler");
    expect(bits(result[6])==0x80000000 && bits(result[8])==0x80000000,
        "paired negative multiply-subtract preserves negative zero after rounding");
    p=pose(4);p[4]=p[5]=0;p[6]=p[7]=bits(1);
    expect(awl::prepare_world_map_model_pose(p,&result)==Status::Prepared && close(result,{0,-1,0,0,1,0,0,0,0,0,1,0}),
        "Z-axis quaternion gives independently known positive quarter-turn orientation");
    for (unsigned fault=0;fault<6;++fault) {
        p=pose();const auto before=result;
        switch(fault) {
        case 0:p[4]=p[5]=p[6]=p[7]=0;break;
        case 1:p[4]=bits(std::numeric_limits<float>::infinity());break;
        case 2:p[1]=0x7fc00000;break;
        case 3:p[8]=0x7fc00000;break;
        case 4:p[4]=bits(std::numeric_limits<float>::max());break;
        case 5:p[4]=bits(std::numeric_limits<float>::denorm_min());break;
        }
        expect(awl::prepare_world_map_model_pose(p,&result)==Status::InvalidInput && result==before,
            "degenerate, reached nonfinite, overflowing and underflowing quaternion input preserves output");
    }
    p=pose(16);p[12]=0x7fc00000;
    expect(awl::prepare_world_map_model_pose(p,&result)==Status::InvalidInput &&
        awl::prepare_world_map_model_pose(p,nullptr)==Status::InvalidInput,"explicit matrix finite validation and null output reject");
}
void test_resource() {
    awl::WorldMapModelBank bank;auto bytes=archive();expect(bank.parse(0x100000003ull,bytes),"owned synthetic bank parses");
    awl::WorldMapModelPreparationStep step;
    expect(bank.prepare(1,&step)==Status::Prepared && step.prepared && step.prepared->records.size()==2,
        "complete owned resource preparation succeeds");
    if(!step.prepared)return;
    const auto& prepared=*step.prepared;
    expect(prepared.pointer_c->offset==96 && prepared.pointer_10->offset==304 && prepared.pointer_1c->offset==308 &&
        prepared.word_18==5 && prepared.word_1c==244 && prepared.records[0].pointers[0]->offset==160 &&
        prepared.records[0].pointers[0]->bank_identity==0x100000003ull && !prepared.records[0].pointers[2] &&
        prepared.records[0].pointers[4]->offset==319 && prepared.records[0].word_14==0xfeedbeef &&
        prepared.records[0].word_18==0x87654321 && close(*prepared.records[0].matrix,{2,0,0,7,0,3,0,8,0,0,-4,9}),
        "references keep full owner identity, opaque words, null offsets and file-relative conversion");
    expect(bank.prepare(1,&step)==Status::Prepared && close(*step.prepared->records[0].matrix,{2,0,0,7,0,3,0,8,0,0,-4,9}),
        "repeated preparation is stable without corrupting serialized bytes or adding base twice");
    for (unsigned fault=0;fault<9;++fault) {
        bytes=archive();Status expected=Status::InvalidInput;
        switch(fault) {
        case 0:put(bytes,64+12,256);break;
        case 1:put(bytes,64+16,UINT32_MAX);break;
        case 2:put(bytes,64+28,256);break;
        case 3:put(bytes,64+32,208);break; // 52 bytes do not fit.
        case 4:put(bytes,64+36,256);break;
        case 5:put(bytes,64+32,32);expected=Status::UnsupportedLayout;break;
        case 6:put(bytes,64+32,97);expected=Status::UnsupportedLayout;break;
        case 7:put(bytes,64+60,96);expected=Status::UnsupportedLayout;break;
        case 8:put(bytes,64+60,100);expected=Status::UnsupportedLayout;break;
        }
        expect(bank.parse(7,bytes),"invalid reached payload still has supported archive metadata");
        step.required_record_index=777;
        expect(bank.prepare(1,&step)==expected && step.required_record_index==777,
            "bounds and unsupported overlap/alignment failures preserve complete output");
    }
    bytes=archive();put(bytes,64+24,0);put(bytes,64+28,UINT32_MAX);put(bytes,64+32,0);
    expect(bank.parse(7,bytes) && bank.prepare(1,&step)==Status::Prepared &&
        !step.prepared->pointer_1c && step.prepared->word_1c==UINT32_MAX && !step.prepared->records[0].matrix,
        "zero header gate preserves opaque out-of-range word and null pose skips conversion");
    bytes=archive();put(bytes,64+148,2u<<24);put(bytes,64+148+16,bits(30));
    expect(bank.parse(7,bytes) && bank.prepare(1,&step)==Status::RequiresEulerRotation &&
        !step.prepared && step.required_record_index==1,"later Euler stop returns no accepted partial resource");
    awl::WorldMapSecondarySetupStep setup;
    expect(awl::prepare_world_map_secondary_model_setup(1,7,&bank,0,std::nullopt,{},900,&setup)==
        awl::WorldMapSecondarySetupStatus::RequiresResourcePreparation &&
        setup.construction->preparation_status==Status::RequiresEulerRotation && !setup.construction->preparation.prepared,
        "secondary construction waits for unsupported reached pose conversion");
    expect(bank.prepare(0,&step)==Status::InvalidInput && bank.prepare(1,nullptr)==Status::InvalidInput,
        "root index and null preparation output reject");
}
void test_pose_matrix() {
    // Independent geometric reference: rotate each basis vector by the
    // quaternion sandwich using two cross products in double precision.
    // This does not duplicate the translated SDK's paired-single algebra.
    for(uint32_t seed=0;seed<2048;++seed) {
        const uint32_t flags=seed&31;
        std::array<float,12> v{2,-3,4,static_cast<float>((seed>>3)&7)-3.5f,
            static_cast<float>((seed>>6)&7)-3.5f,static_cast<float>((seed>>9)&3)-1.5f,2,7,8,9,11,12};
        if((flags&2) && !(flags&20))v[3]=v[4]=v[5]=0;
        Pose p{};p[0]=flags<<24;for(size_t i=0;i<v.size();++i)p[i+1]=bits(v[i]);
        Matrix expected{1,0,0,0,0,1,0,0,0,0,1,0},actual{};
        if(flags&16)expected=v;
        else {
            if(flags&4) {
                const std::array<double,3> q{v[3],v[4],v[5]};const double w=v[6];
                const double norm=q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+w*w;
                auto cross=[](const std::array<double,3>& a,const std::array<double,3>& b) {
                    return std::array<double,3>{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
                };
                for(size_t col=0;col<3;++col) {
                    std::array<double,3> basis{};basis[col]=1;
                    const auto first=cross(q,basis),second=cross(q,first);
                    for(size_t row=0;row<3;++row) expected[row*4+col]=
                        static_cast<float>(basis[row]+2.0*(w*first[row]+second[row])/norm);
                }
            }
            if(flags&1)for(size_t row=0;row<3;++row)for(size_t col=0;col<3;++col)expected[row*4+col]*=v[col];
            if(flags&8)for(size_t row=0;row<3;++row)expected[row*4+3]=v[row+7];
        }
        expect(awl::prepare_world_map_model_pose(p,&actual)==Status::Prepared && close(actual,expected),
            "2,048 flag/quaternion cases agree with an independent geometric reference");
    }
}
void test_resource_matrix() {
    uint64_t digest=14695981039346656037ull;
    auto hash=[&](uint32_t word) {for(unsigned i=0;i<4;++i){digest^=(word>>(24-i*8))&255;digest*=1099511628211ull;}};
    for(uint32_t seed=0;seed<1024;++seed) {
        auto bytes=archive();
        put(bytes,64+12,seed&1?32u:0u);put(bytes,64+16,seed&2?240u:0u);
        put(bytes,64+24,seed&4?5u:0u);put(bytes,64+28,seed&8?244u:0u);
        for(unsigned i=0;i<2;++i) {
            const size_t start=64+32+28*i;
            put(bytes,start,seed&(16u<<i)?96u+52u*i:0u);
            put(bytes,start+4,seed&64?240u:0u);put(bytes,start+8,seed&128?241u:0u);
            put(bytes,start+12,seed&256?32u:0u);put(bytes,start+16,seed&512?255u:0u);
        }
        awl::WorldMapModelBank bank;awl::WorldMapModelPreparationStep step;
        expect(bank.parse(7,std::move(bytes)) && bank.prepare(1,&step)==Status::Prepared,"full fixup matrix prepares");
        if(!step.prepared)continue;
        const auto& r=*step.prepared;
        auto offset=[](const std::optional<awl::WorldMapModelResourceReference>& p){return p?p->offset-64:0;};
        hash(seed);hash(offset(r.pointer_c));hash(offset(r.pointer_10));hash(offset(r.pointer_1c));hash(r.word_18);hash(r.word_1c);
        for(const auto& record:r.records) {
            for(const auto& p:record.pointers)hash(offset(p));
            hash(record.word_14);hash(record.word_18);hash(record.matrix?1u:0u);
        }
    }
    std::cout<<"MODEL_RESOURCE_POINTER_MATRIX 1024 "<<std::hex<<digest<<std::dec<<'\n';
    expect(digest==0xcfab4e52038e1265ull,"1,024 full pointer/gate/null/untouched-word cases match mapped original instructions");
}
void instruction_cases(const std::filesystem::path& path) {
    std::ifstream input(path);unsigned cases=0;float maximum=0;
    for (;;) {
        Pose p{};Matrix expected{},actual{};uint32_t word;
        if(!(input>>std::hex>>p[0]))break;
        bool complete=true;for(size_t i=1;i<p.size();++i)if(!(input>>std::hex>>p[i]))complete=false;
        for(auto& v:expected){if(!(input>>std::hex>>word))complete=false;else v=value(word);}
        expect(complete,"independent instruction case is complete");if(!complete)break;
        expect(awl::prepare_world_map_model_pose(p,&actual)==Status::Prepared && close(actual,expected),
            "native pose matches mapped instruction walk within declared reciprocal tolerance");
        for(size_t i=0;i<12;++i)maximum=std::max(maximum,std::abs(actual[i]-expected[i])/std::max(1.0f,std::abs(expected[i])));
        ++cases;
    }
    std::cout<<"MODEL_POSE_INSTRUCTIONS "<<cases<<" maximum relative/absolute error "<<maximum<<'\n';
    expect(cases==6144,"all independent mapped instruction cases were compared");
}
void local_resource(const std::filesystem::path& disc, const std::filesystem::path& comparison={}) {
    std::ifstream input(disc/"files"/"boy_0.arc",std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});awl::WorldMapModelBank bank;
    awl::WorldMapModelPreparationStep step;
    expect(bank.parse(300,std::move(bytes)) && bank.prepare(1,&step)==Status::Prepared,
        "local diagnostic ACT prepares without unsupported offsets, overlapping poses or rotations");
    if(!step.prepared)return;
    size_t matrices=0;for(const auto& record:step.prepared->records)if(record.matrix)++matrices;
    std::cout<<"LOCAL_MODEL_PREPARATION "<<step.prepared->records.size()<<' '<<matrices<<'\n';
    expect(step.prepared->records.size()==55 && matrices==55,"all 55 reached diagnostic poses produce matrices");
    if(!comparison.empty()) {
        std::ifstream expected_input(comparison);uint32_t index,word;unsigned cases=0;float maximum=0;
        while(expected_input>>std::dec>>index) {
            Matrix expected{};bool complete=true;
            for(auto& v:expected){if(!(expected_input>>std::hex>>word))complete=false;else v=value(word);}
            expect(complete && index<step.prepared->records.size(),"local mapped matrix case has valid record and full payload");
            if(!complete || index>=step.prepared->records.size())break;
            const auto& matrix=step.prepared->records[index].matrix;
            expect(matrix && close(*matrix,expected),"local native matrix agrees with full mapped resource preparation within reciprocal tolerance");
            if(matrix)for(size_t i=0;i<12;++i)maximum=std::max(maximum,std::abs((*matrix)[i]-expected[i])/std::max(1.0f,std::abs(expected[i])));
            ++cases;
        }
        std::cout<<"LOCAL_MODEL_MAPPED_MATRICES "<<cases<<" maximum relative/absolute error "<<maximum<<'\n';
        expect(cases==165,"all 55 local records compared under three supplied reciprocal estimates");
    }
}
} // namespace
int main(int argc,char** argv) {
    test_pose();test_resource();test_pose_matrix();test_resource_matrix();
    if(argc==3 && std::string(argv[1])=="--pose-instructions")instruction_cases(argv[2]);
    else if(argc==4 && std::string(argv[1])=="--model-resource-local")local_resource(argv[2],argv[3]);
    else if(argc==3 && std::string(argv[1])=="--model-resource-local")local_resource(argv[2]);
    else if(argc!=1)expect(false,"usage: --pose-instructions <ignored probe> | --model-resource-local <disc> [ignored comparison]");
    return failures==0?0:1;
}
