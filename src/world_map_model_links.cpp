#include "awl/world_map_model_links.h"

#include <unordered_map>
#include <utility>

namespace awl {
WorldMapModelLinkStatus prepare_world_map_model_links_clear(
    const WorldMapModelLinkState& state, uint64_t root, WorldMapModelLinkStep* out) {
    using Status = WorldMapModelLinkStatus;
    if (out == nullptr || &state == &out->after || root == 0) return Status::InvalidInput;
    std::unordered_map<uint64_t, size_t> indices;
    for (size_t i = 0; i < state.nodes.size(); ++i) {
        if (state.nodes[i].identity == 0 || !indices.emplace(state.nodes[i].identity, i).second) return Status::InvalidInput;
    }
    WorldMapModelLinkStep step;
    step.after = state;
    auto missing = [&](uint64_t identity) {
        step.after = state; step.writes.clear(); step.required_node = identity;
        *out = std::move(step); return Status::RequiresNode;
    };
    const auto first = indices.find(root);
    if (first == indices.end()) return missing(root);
    struct Frame { size_t node = 0, slot = 0; bool awaiting_child = false; };
    std::vector<Frame> stack{{first->second, 0, false}};
    std::vector<bool> active(state.nodes.size(), false);
    active[first->second] = true;
    auto clear_slot = [&](WorldMapModelLinkNode& node, size_t slot) {
        node.children_15c[slot] = 0;
        step.writes.push_back({node.identity, 0x15cu + static_cast<uint32_t>(slot) * 4u, 0});
    };
    while (!stack.empty()) {
        auto& frame = stack.back();
        auto& node = step.after.nodes[frame.node];
        if (frame.awaiting_child) {
            clear_slot(node, frame.slot);
            frame.awaiting_child = false; ++frame.slot;
        } else if (frame.slot == 4) {
            active[frame.node] = false; stack.pop_back();
        } else if (node.children_15c[frame.slot] == 0) {
            ++frame.slot;
        } else {
            const auto found = indices.find(node.children_15c[frame.slot]);
            if (found == indices.end()) return missing(node.children_15c[frame.slot]);
            auto& child = step.after.nodes[found->second];
            child.parent_150 = 0;
            step.writes.push_back({child.identity, 0x150, 0});
            if (!child.flags_158) {
                const auto identity = child.identity;
                step.after = state; step.writes.clear(); step.required_node = identity;
                *out = std::move(step); return Status::RequiresFlags;
            }
            if ((*child.flags_158 & 0x4u) != 0) {
                if (active[found->second]) return Status::InvalidInput;
                frame.awaiting_child = true;
                active[found->second] = true;
                stack.push_back({found->second, 0, false});
            } else {
                clear_slot(node, frame.slot); ++frame.slot;
            }
        }
    }
    *out = std::move(step);
    return Status::Prepared;
}

WorldMapModelLinkStatus advance_world_map_model_links_clear(
    WorldMapModelLinkState* state, uint64_t root, WorldMapModelLinkStep* out) {
    if (state == nullptr || out == nullptr || state == &out->after) return WorldMapModelLinkStatus::InvalidInput;
    const auto status = prepare_world_map_model_links_clear(*state, root, out);
    if (status != WorldMapModelLinkStatus::Prepared) return status;
    *state = out->after;
    return WorldMapModelLinkStatus::Advanced;
}
WorldMapModelAttachmentStatus prepare_world_map_model_attachments(
    const WorldMapModelLinkState& state,const WorldMapModelAttachmentRequest& request,
    const std::vector<WorldMapModelAttachmentBinding>& bindings,WorldMapModelAttachmentStep* out) {
    using Status=WorldMapModelAttachmentStatus;
    if(out==nullptr || &state==&out->after)return Status::InvalidInput;
    for(size_t i=0;i<state.nodes.size();++i) {
        if(state.nodes[i].identity==0)return Status::InvalidInput;
        for(size_t j=0;j<i;++j)if(state.nodes[i].identity==state.nodes[j].identity)return Status::InvalidInput;
    }
    for(size_t i=0;i<bindings.size();++i) {
        if(bindings[i].model==0)return Status::InvalidInput;
        for(size_t j=0;j<i;++j)if(bindings[i].model==bindings[j].model)return Status::InvalidInput;
    }
    WorldMapModelAttachmentStep step;step.after=state;
    auto stop=[&](Status status,uint64_t model=0) {
        step.after=state;step.writes.clear();step.auxiliary_slot.reset();step.secondary_slot.reset();
        step.required_model=model;*out=std::move(step);return status;
    };
    auto find=[&](uint64_t identity)->WorldMapModelLinkNode* {
        for(auto& node:step.after.nodes)if(node.identity==identity)return &node;
        return nullptr;
    };
    auto binding=[&](uint64_t identity)->const WorldMapModelAttachmentBinding* {
        for(const auto& row:bindings)if(row.model==identity)return &row;
        return nullptr;
    };
    if(request.primary_model!=0) {
        WorldMapModelLinkStep cleared;
        const auto status=prepare_world_map_model_links_clear(state,request.primary_model,&cleared);
        if(status==WorldMapModelLinkStatus::InvalidInput)return Status::InvalidInput;
        if(status!=WorldMapModelLinkStatus::Prepared) {
            if(status==WorldMapModelLinkStatus::RequiresFlags)step.required_field=0x158;
            return stop(status==WorldMapModelLinkStatus::RequiresFlags?Status::RequiresFlags:Status::RequiresNode,cleared.required_node);
        }
        step.after=std::move(cleared.after);step.writes=std::move(cleared.writes);
    }
    if(request.secondary_model==0){*out=std::move(step);return Status::Prepared;}
    if(!request.feature_c4){step.required_field=0xc4;return stop(Status::RequiresSource);}
    if(*request.feature_c4!=0) {
        const auto* row=binding(request.secondary_model);
        if(!row || !row->first_node_index)return stop(Status::RequiresNodeIndex,request.secondary_model);
        step.feature_node_index=row->first_node_index;step.required_source=*request.feature_c4;
        return stop(Status::RequiresFeatureBinding,request.secondary_model);
    }
    if(!request.auxiliary_model_c8){step.required_field=0xc8;return stop(Status::RequiresSource);}
    const auto auxiliary=*request.auxiliary_model_c8;
    if(auxiliary==0){*out=std::move(step);return Status::Prepared;}
    // DFCC retains r3 from DF8C: DB4C scans secondary, not the C8 child.
    const auto* secondary_binding=binding(request.secondary_model);
    if(!secondary_binding || !secondary_binding->first_node_index)return stop(Status::RequiresNodeIndex,request.secondary_model);
    auto attach=[&](uint64_t parent,uint64_t child,uint16_t index,std::optional<uint32_t>& slot) {
        if(parent==0 || child==0)return Status::InvalidInput;
        auto* node=find(parent);if(!node){step.required_model=parent;return Status::RequiresNode;}
        for(uint32_t i=0;i<4;++i)if(node->children_15c[i]==0) {
            auto* attached=find(child);if(!attached){step.required_model=child;return Status::RequiresNode;}
            attached->parent_150=parent;step.writes.push_back({child,0x150,parent});
            node->children_15c[i]=child;step.writes.push_back({parent,0x15c+i*4,child});
            node->attachments_16c[i]=index;step.writes.push_back({parent,0x16c+i*2,index});
            attached->flags_158=15;step.writes.push_back({child,0x158,15});slot=i;break;
        }
        return Status::Prepared;
    };
    auto status=attach(request.secondary_model,auxiliary,*secondary_binding->first_node_index,step.auxiliary_slot);
    if(status==Status::InvalidInput)return status;
    if(status!=Status::Prepared)return stop(status,step.required_model);
    auto* auxiliary_node=find(auxiliary);
    if(!auxiliary_node)return stop(Status::RequiresNode,auxiliary);
    if(!auxiliary_node->flags_158){step.required_field=0x158;return stop(Status::RequiresFlags,auxiliary);}
    auxiliary_node->flags_158=*auxiliary_node->flags_158&~3u;
    step.writes.push_back({auxiliary,0x158,*auxiliary_node->flags_158});
    if(!secondary_binding || !secondary_binding->resource_attachment_index)
        return stop(Status::RequiresAttachmentIndex,request.secondary_model);
    status=attach(request.primary_model,request.secondary_model,*secondary_binding->resource_attachment_index,step.secondary_slot);
    if(status==Status::InvalidInput)return status;
    if(status!=Status::Prepared)return stop(status,step.required_model);
    *out=std::move(step);return Status::Prepared;
}
} // namespace awl
