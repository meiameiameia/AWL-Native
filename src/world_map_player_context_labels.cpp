#include "awl/world_map_player_context_labels.h"
#include "awl/disc_identity.h"
#include "awl/filesystem.h"
#include "world_map_dol_view.h"
#include <cfenv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <new>
#include <utility>

namespace awl {
namespace {
using S=WorldMapPlayerContextLabelStatus;
using T=WorldMapPlayerContextLabelTablesStatus;
bool ordinary(float value) noexcept {return std::isfinite(value) && std::fpclassify(value)!=FP_SUBNORMAL;}
uint32_t flag_class(uint8_t flags) noexcept {
    if(flags&1)return 0;if(flags&4)return 2;if(flags&8)return 3;
    if(flags&16)return 4;if(flags&2)return 1;if(flags&32)return 5;
    return flags&64?6u:0u;
}
S item_class(const WorldMapPlayerContextLabelTables& tables,const WorldMapPlayerContextLabelInput& input,uint32_t& out) noexcept {
    const auto item=input.value;
    if(item==0){out=1;return S::Prepared;}
    if(item<0)return S::InvalidInput;
    if(!input.item_row)return S::RequiresItemRow;
    const auto& row=*input.item_row;if(row.item_id!=item)return S::InvalidInput;
    if(!row.type_0)return S::RequiresItemType;const auto type=*row.type_0;
    if((type==8 || type==9) && (!input.item_record_resolved || !*input.item_record_resolved))return S::RequiresItemRecord;
    if(item==30){out=29;return S::Prepared;}if(type==29){out=30;return S::Prepared;}
    if(item==2){out=28;return S::Prepared;}if(item==3){out=26;return S::Prepared;}if(item==4){out=27;return S::Prepared;}
    if(type==21){out=25;return S::Prepared;}if(item==6){out=16;return S::Prepared;}if(type==19){out=22;return S::Prepared;}
    if(!row.flags_1)return S::RequiresItemFlags;
    if(flag_class(*row.flags_1)==4){out=23;return S::Prepared;}if(type==8){out=24;return S::Prepared;}
    if(type==9){if(!input.item_record_byte_c)return S::RequiresItemRecordByte;
        if(*input.item_record_byte_c==2){out=15;return S::Prepared;}}
    if(!row.flags_1c)return S::RequiresItemFlags;
    if(*row.flags_1c&6){out=21;return S::Prepared;}
    if(item==0x4f6 || item==0x4f7){out=9;return S::Prepared;}
    if(item==0x4f8 || item==0x4f9){out=10;return S::Prepared;}
    if(item==0x459){out=18;return S::Prepared;}
    out=tables.item_class(type);return S::Prepared;
}
S target_class(const WorldMapPlayerContextLabelInput& input,uint32_t& out) noexcept {
    const uint32_t group=input.direction==1?2u:input.direction==2 || input.direction==3?1u:0u;
    const int32_t value=input.target_payload;out=0;
    // Verified 802A7B84 routes, translated as control flow. Payload tests
    // retain signed comparisons; direction is the original unsigned word.
    switch(input.target_kind){
    case 0:out=value==16?39u:40u;break;
    case 1:out=6;break;
    case 2:case 3:if(value!=-1)out=group==1 && value==71?5u:group+2;break;
    case 4:out=group+7;break;
    case 5:case 6:case 9:case 10:if(value==69)out=19;else if(value==68)out=18;break;
    case 7:out=group+11;break;
    case 8:out=group==1 && value==75?17u:group+14;break;
    case 11:out=81;break;case 12:out=82;break;
    case 14:if(value>=3 && value<=8){switch(value){case 3:out=20;break;case 4:out=22;break;case 5:out=24;break;
        case 6:out=25;break;case 7:out=21;break;case 8:out=23;break;}}break;
    case 15:if(value==9)out=26;else if(value==10)out=24;break;
    case 16:if(value==11)out=27;else if(value==12)out=28;break;
    case 17:out=77;break;
    case 19:if(value==19)out=29;else if(value==20 || value==21)out=30;break;
    case 20:if(value>=22 && value<=31){if(value<=23)out=31;else if(value<=26)out=32;
        else if(value==27 || value>=29)out=33;else out=34;}break;
    case 21:if(value>=32 && value<=39){if(value<=33)out=31;else if(value<=36)out=32;
        else if(value==39)out=33;else out=34;}break;
    case 22:if(value>=40 && value<=44)out=value<=41?31u:value<=43?35u:36u;break;
    case 23:out=38;break;case 24:out=61;break;case 25:case 26:out=41;break;
    case 27:if(value==51)out=49;else if(value==50)out=50;break;
    case 28:out=52;break;
    case 30:case 31:
        if(!input.target_record_resolved || !*input.target_record_resolved)return S::RequiresTargetRecord;
        if(!input.target_record_predicate)return S::RequiresTargetPredicate;
        if(input.target_kind==31){out=*input.target_record_predicate?59u:60u;break;}
        if(!input.target_record_flags_28)return S::RequiresTargetFlags;
        if(!(*input.target_record_flags_28&1))out=*input.target_record_predicate?57u:58u;
        else if(*input.target_record_flags_28&2)out=*input.target_record_predicate?53u:54u;
        else out=*input.target_record_predicate?55u:56u;break;
    case 32:out=63;break;
    case 33:if(value==51)out=65;else if(value==50)out=64;break;
    case 34:out=74;break;case 35:out=75;break;case 36:out=66;break;case 37:out=67;break;case 38:out=68;break;
    case 39:if(value==56)out=69;else if(value==57)out=70;break;
    case 40:if(value==58)out=72;else if(value==59)out=71;break;
    case 41:if(value==60)out=43;else if(value==61)out=44;break;
    case 42:if(value==62)out=45;else if(value==63)out=46;break;
    case 43:out=73;break;case 44:out=79;break;case 45:out=80;break;case 46:out=78;break;
    case 47:if(value==64)out=47;else if(value==65)out=48;break;
    case 48:if(value==66)out=83;else if(value==67)out=84;break;
    case 49:out=85;break;case 50:out=62;break;
    default:break;
    }
    return S::Prepared;
}
}
bool WorldMapPlayerContextLabelTables::decode(const uint8_t* data,size_t size) noexcept {
    detail::DolView view;if(!view.initialize(data,size))return false;
    const auto* rows=view.read(0x80292a78,31*172);const auto* jump=view.read(0x802a7b08,31*4);const auto* distance=view.read(0x8034a3c8,4);
    if(!rows || !jump || !distance)return false;
    for(size_t i=0;i<rows_.size();++i)std::memcpy(rows_[i].data(),rows+i*172,172);
    for(size_t i=0;i<item_classes_.size();++i){const uint32_t address=detail::dol_word(jump+i*4);
        if(address<0x8004f19c || address>0x8004f1f4 || ((address-0x8004f19c)&7))return false;
        const auto* instruction=view.read(address,4);if(!instruction)return false;
        const uint32_t word=detail::dol_word(instruction);if((word&0xffff0000u)!=0x38600000u || (word&0xffff)>30)return false;
        item_classes_[i]=static_cast<uint8_t>(word&0xffff);}
    const uint32_t bits=detail::dol_word(distance);std::memcpy(&probe_distance_,&bits,4);
    // Label-only fixtures remain valid. Missing icon metadata cannot authorize
    // a later mapped-icon comparison; the icon API reports that dependency.
    if(const auto* icons=view.read(0x8024f694,103)){std::array<uint8_t,103> copy;std::memcpy(copy.data(),icons,copy.size());icons_=copy;}
    return ordinary(probe_distance_) && probe_distance_>0;
}
bool WorldMapPlayerContextLabelTables::icon(uint32_t label,uint8_t* out) const noexcept {
    if(!out || !icons_ || label>=icons_->size())return false;
    *out=(*icons_)[label];return true;
}
bool WorldMapPlayerContextLabelTables::labels(uint32_t classification,uint32_t context,std::array<uint32_t,2>* out) const noexcept {
    if(!out || classification>=rows_.size() || context>=86)return false;
    *out={rows_[classification][context],rows_[classification][86+context]};return true;
}
T decode_world_map_player_context_label_tables(const uint8_t* bytes,size_t size,std::shared_ptr<const WorldMapPlayerContextLabelTables>* out) {
    if(!out)return T::InvalidInput;
    try{auto next=std::shared_ptr<WorldMapPlayerContextLabelTables>(new WorldMapPlayerContextLabelTables);
        if(!next->decode(bytes,size))return T::UnsupportedLayout;*out=std::move(next);return T::Decoded;
    }catch(const std::bad_alloc&){return T::AllocationFailure;}
}
T load_world_map_player_context_label_tables(std::shared_ptr<const WorldMapPlayerContextLabelTables>* out) {
    if(!out)return T::InvalidInput;if(*out && (*out)->target_verified())return T::Loaded;
    try{char path[1024];if(!filesystem_resolve_path("/sys/main.dol",path,sizeof(path)))return T::ReadFailure;
        std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return T::ReadFailure;
        const auto size=input.tellg();if(size<0 || size>static_cast<std::streamoff>(detail::max_dol_size))return T::ReadFailure;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));input.seekg(0);
        if(!input || (size!=0 && !input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) ||
            input.peek()!=std::char_traits<char>::eof() || input.bad())return T::ReadFailure;
        Sha1 sha;if(!sha1_bytes(bytes.data(),bytes.size(),&sha))return T::ReadFailure;if(sha!=kTargetDolSha1)return T::WrongDol;
        auto next=std::shared_ptr<WorldMapPlayerContextLabelTables>(new WorldMapPlayerContextLabelTables);
        if(!next->decode(bytes.data(),bytes.size()))return T::UnsupportedLayout;next->target_verified_=true;*out=std::move(next);return T::Loaded;
    }catch(const std::bad_alloc&){return T::AllocationFailure;}
}
WorldMapPlayerContextLabelStep prepare_world_map_player_context_labels(const WorldMapPlayerContextLabelTables* tables,
    const WorldMapPlayerContextLabelInput& input,WorldMapPlayerContextLabels* out) noexcept {
    WorldMapPlayerContextLabelStep step;if(!out)return step;
    if(input.source!=WorldMapPlayerContextLabelSource::HeldItem && input.source!=WorldMapPlayerContextLabelSource::Action)return step;
    if(!tables){step.status=S::RequiresTables;return step;}
    WorldMapPlayerContextLabels next;next.scene_type=input.scene_type;next.position=input.position;next.facing=input.facing;
    next.target_kind=input.target_kind;next.target_payload=input.target_payload;next.direction=input.direction;
    if(input.source==WorldMapPlayerContextLabelSource::HeldItem){next.item=input.value;step.status=item_class(*tables,input,step.classification);if(step.status!=S::Prepared)return step;}
    else {next.action=input.value;step.classification=input.value==2?2u:input.value>=0x30 && input.value<0x38?3u:input.value>=0x38 && input.value<0x3d?4u:0u;}
    if(input.query_mode!=1){step.status=target_class(input,step.context);if(step.status!=S::Prepared)return step;}
    if(step.context==74 || step.context==75){const auto slot=step.context-74;
        if(!input.context_item_available[slot]){step.status=S::RequiresItemAvailability;return step;}
        if(!*input.context_item_available[slot])step.context=76;}
    if(step.context==8){if(!input.context8_scene_blocked){step.status=S::RequiresScenePredicate;return step;}
        if(*input.context8_scene_blocked)step.context=0;}
    std::array<uint32_t,2> labels;if(!tables->labels(step.classification,step.context,&labels)){step.status=S::InvalidInput;return step;}
    next.labels={labels[0],labels[1],step.context==42?102u:next.action==0?101u:0u};
    if(next.labels[1]==25){step.probe_mask|=4;
        if(!input.secondary_ground_available){step.status=S::RequiresGround;return step;}
        if(!*input.secondary_ground_available)next.labels[1]=0;}
    for(uint32_t channel=0;channel<2;++channel)if(next.labels[channel]==29){
        step.probe_mask|=static_cast<uint8_t>(1u<<channel);
        step.terrain_channel=channel;WorldMapPlayerContextVector position;
        if(std::fegetround()!=FE_TONEAREST){step.status=S::UnsupportedNumerics;return step;}
        for(size_t axis=0;axis<3;++axis){const float offset=input.facing[axis]*tables->probe_distance();position[axis]=input.position[axis]+offset;
            if(!ordinary(input.position[axis]) || !ordinary(input.facing[axis]) || !ordinary(offset) || !ordinary(position[axis])){step.status=S::UnsupportedNumerics;return step;}}
        step.terrain_position=position;if(!input.terrain_clear[channel]){step.status=S::RequiresTerrain;return step;}
        if(!*input.terrain_clear[channel])next.labels[channel]=0;
    }
    if(step.context==47){
        if(!input.item_row){step.status=S::RequiresItemRow;return step;}
        if(input.item_row->item_id!=next.item){step.status=S::InvalidInput;return step;}
        if(!input.item_row->flags_1c){step.status=S::RequiresItemFlags;return step;}
        if(*input.item_row->flags_1c&2){if(!input.item_flag1_available){step.status=S::RequiresItemAvailability;return step;}
            if(*input.item_flag1_available)next.labels[0]=35;}
    }
    if(input.source==WorldMapPlayerContextLabelSource::Action && input.target_kind==30){
        if(!input.action_record_resolved || !*input.action_record_resolved){step.status=S::RequiresActionRecord;return step;}
        uint8_t mask=0;
        if(input.value>=0x30 && input.value<0x38)mask=8;
        else if(input.value==0x38 || input.value==0x39 || input.value==0x3c)mask=4;
        else if(input.value==0x3a || input.value==0x3b)mask=32;
        if(mask){if(!input.action_record_flags_28){step.status=S::RequiresActionFlags;return step;}
            if(!(*input.action_record_flags_28&mask))++next.labels[0];}
    }
    step.status=S::Prepared;*out=next;return step;
}
WorldMapPlayerContextLabelStep prepare_world_map_player_context_labels_from_target(const WorldMapPlayerContextLabelTables* tables,
    const WorldMapPlayerContextResult& target,const WorldMapPlayerContextLabelInput& input,WorldMapPlayerContextLabels* out) noexcept {
    auto bound=input;
    std::memcpy(&bound.target_kind,&target.record.payload[0],4);
    std::memcpy(&bound.target_payload,&target.record.payload[1],4);
    bound.direction=target.record.direction;
    return prepare_world_map_player_context_labels(tables,bound,out);
}
WorldMapPlayerContextIconStep prepare_world_map_player_context_icon(const WorldMapPlayerContextLabelTables* tables,
    const WorldMapPlayerContextIconState& input,uint32_t channel,uint32_t label,WorldMapPlayerContextIconState* out) noexcept {
    using I=WorldMapPlayerContextIconStatus;WorldMapPlayerContextIconStep step;step.channel=channel;
    if(!out || channel>=4)return step;
    if(!input.resource_manager_1278){step.status=I::RequiresResourceManager;return step;}
    if(!input.labels_60[channel]){step.status=I::RequiresPreviousLabel;return step;}
    const uint32_t previous=*input.labels_60[channel];
    if(previous!=label){
        if(previous>=103 || label>=103){step.status=I::UnsupportedLabel;return step;}
        uint8_t old_icon,new_icon;
        if(!tables || !tables->icon(label,&new_icon) || !tables->icon(previous,&old_icon)){step.status=I::RequiresIconMap;return step;}
        step.previous_icon=old_icon;step.next_icon=new_icon;
        if(old_icon!=new_icon){step.status=I::RequiresIconAssets;return step;}
    }
    auto next=input;next.labels_60[channel]=label;*out=next;step.prepared_mask=1u<<channel;step.status=I::Prepared;return step;
}
WorldMapPlayerContextIconStep prepare_world_map_player_context_icons(const WorldMapPlayerContextLabelTables* tables,
    const WorldMapPlayerContextIconState& input,const WorldMapPlayerContextLabels& labels,WorldMapPlayerContextIconState* out) noexcept {
    using I=WorldMapPlayerContextIconStatus;WorldMapPlayerContextIconStep step;if(!out)return step;
    auto next=input;uint32_t mask=0;
    for(const auto channel:{0u,1u,3u,2u}){
        const auto label=channel==0?labels.labels[0]:channel==1?0u:channel==3?labels.labels[1]:labels.labels[2];
        step=prepare_world_map_player_context_icon(tables,next,channel,label,&next);step.prepared_mask|=mask;
        if(step.status!=I::Prepared)return step;mask=step.prepared_mask;
    }
    *out=next;return step;
}
} // namespace awl
