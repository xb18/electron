// Copyright (c) 2026 Anthropic, PBC.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

// The ordering part of Menu.buildFromTemplate().

#include <deque>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

#include "gin/converter.h"
#include "gin/dictionary.h"
#include "shell/browser/api/electron_api_menu.h"
#include "shell/common/gin_helper/dictionary.h"
#include "v8/include/v8.h"

namespace electron::api {

namespace {

struct TemplateEntry {
  bool is_plain_separator() const { return separator && !constrained; }

  v8::Local<v8::Object> value;
  bool separator = false;
  bool hidden = false;  // visible === false
  // Ids are compared with ===.
  v8::Local<v8::Value> id;
  std::vector<v8::Local<v8::Value>> before, after, before_group, after_group;
  bool constrained = false;  // any of before/after/...GroupContaining set
};

// Array.isArray()
bool IsArray(v8::Local<v8::Value> value) {
  while (value->IsProxy())
    value = value.As<v8::Proxy>()->GetTarget();
  return value->IsArray();
}

uint32_t ArrayLength(v8::Isolate* isolate, v8::Local<v8::Object> array) {
  uint32_t length = 0;
  gin_helper::Dictionary(isolate, array).Get("length", &length);
  return length;
}

std::vector<v8::Local<v8::Value>> IdList(v8::Isolate* isolate,
                                         const gin_helper::Dictionary& dict,
                                         std::string_view key,
                                         bool* constrained) {
  std::vector<v8::Local<v8::Value>> out;
  v8::Local<v8::Value> value;
  if (!dict.Get(key, &value))
    return out;
  if (value->BooleanValue(isolate))
    *constrained = true;
  if (!IsArray(value))
    return out;
  v8::Local<v8::Object> array = value.As<v8::Object>();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  for (uint32_t i = 0, n = ArrayLength(isolate, array); i < n; ++i) {
    v8::Local<v8::Value> element;
    if (array->Get(context, i).ToLocal(&element))
      out.push_back(element);
  }
  return out;
}

bool SameId(v8::Local<v8::Value> a, v8::Local<v8::Value> b) {
  return !a.IsEmpty() && !b.IsEmpty() && a->StrictEquals(b);
}

TemplateEntry ReadEntry(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  gin_helper::Dictionary dict(isolate, object);
  TemplateEntry e;
  e.value = object;
  std::string type;
  e.separator = dict.Get("type", &type) && type == "separator";
  v8::Local<v8::Value> visible;
  e.hidden = dict.Get("visible", &visible) && visible->IsFalse();
  v8::Local<v8::Value> id;
  if (dict.Get("id", &id) && !id->IsUndefined())
    e.id = id;
  e.before = IdList(isolate, dict, "before", &e.constrained);
  e.after = IdList(isolate, dict, "after", &e.constrained);
  e.before_group =
      IdList(isolate, dict, "beforeGroupContaining", &e.constrained);
  e.after_group = IdList(isolate, dict, "afterGroupContaining", &e.constrained);
  return e;
}

using Group = std::vector<const TemplateEntry*>;

int IndexOfGroupContainingId(const std::vector<Group>& groups,
                             v8::Local<v8::Value> id,
                             const Group* ignore) {
  for (size_t i = 0; i < groups.size(); ++i) {
    if (&groups[i] == ignore)
      continue;
    for (const TemplateEntry* e : groups[i]) {
      if (SameId(e->id, id))
        return static_cast<int>(i);
    }
  }
  return -1;
}

// Sort nodes topologically, depth first; cycles are broken.
std::vector<int> SortTopologically(
    int count,
    const std::map<int, std::vector<int>>& edges) {
  std::vector<int> sorted;
  std::vector<bool> marked(count, false);
  // (node, index of the next edge to visit)
  std::vector<std::pair<int, size_t>> stack;
  for (int start = 0; start < count; ++start) {
    if (marked[start])
      continue;
    marked[start] = true;
    stack.emplace_back(start, 0);
    while (!stack.empty()) {
      auto& [node, next] = stack.back();
      auto it = edges.find(node);
      if (it != edges.end() && next < it->second.size()) {
        int to = it->second[next++];
        if (!marked[to]) {
          marked[to] = true;
          stack.emplace_back(to, 0);
        }
        continue;
      }
      sorted.push_back(node);
      stack.pop_back();
    }
  }
  return sorted;
}

bool AttemptToMergeAGroup(std::vector<Group>& groups) {
  for (size_t i = 0; i < groups.size(); ++i) {
    Group& group = groups[i];
    for (const TemplateEntry* item : group) {
      std::vector<v8::Local<v8::Value>> to_ids = item->before;
      to_ids.insert(to_ids.end(), item->after.begin(), item->after.end());
      for (v8::Local<v8::Value> id : to_ids) {
        int index = IndexOfGroupContainingId(groups, id, &group);
        if (index == -1)
          continue;
        Group merged = groups[index];
        merged.insert(merged.end(), group.begin(), group.end());
        groups[index] = std::move(merged);
        groups.erase(groups.begin() + i);
        return true;
      }
    }
  }
  return false;
}

Group SortItemsInGroup(const Group& group) {
  // The last item with a given id wins.
  auto index_of = [&](v8::Local<v8::Value> id) -> int {
    for (size_t i = group.size(); i-- > 0;) {
      if (SameId(group[i]->id, id))
        return static_cast<int>(i);
    }
    return -1;
  };
  std::map<int, std::vector<int>> edges;
  for (size_t i = 0; i < group.size(); ++i) {
    for (v8::Local<v8::Value> to_id : group[i]->before) {
      if (int to = index_of(to_id); to != -1)
        edges[to].push_back(static_cast<int>(i));
    }
    for (v8::Local<v8::Value> to_id : group[i]->after) {
      if (int to = index_of(to_id); to != -1)
        edges[static_cast<int>(i)].push_back(to);
    }
  }
  Group sorted;
  for (int index : SortTopologically(static_cast<int>(group.size()), edges))
    sorted.push_back(group[index]);
  return sorted;
}

std::vector<Group> SortGroups(const std::vector<Group>& groups) {
  std::map<int, std::vector<int>> edges;
  for (size_t i = 0; i < groups.size(); ++i) {
    bool found = false;
    for (const TemplateEntry* item : groups[i]) {
      for (v8::Local<v8::Value> id : item->before_group) {
        int to = IndexOfGroupContainingId(groups, id, &groups[i]);
        if (to != -1) {
          edges[to].push_back(static_cast<int>(i));
          found = true;
          break;
        }
      }
      if (found)
        break;
      for (v8::Local<v8::Value> id : item->after_group) {
        int to = IndexOfGroupContainingId(groups, id, &groups[i]);
        if (to != -1) {
          edges[static_cast<int>(i)].push_back(to);
          found = true;
          break;
        }
      }
      if (found)
        break;
    }
  }
  std::vector<Group> sorted;
  for (int index : SortTopologically(static_cast<int>(groups.size()), edges))
    sorted.push_back(groups[index]);
  return sorted;
}

// Applies the before/after/beforeGroupContaining/afterGroupContaining
// constraints. Separators needed between groups beyond those in |entries| are
// appended to |synthesized| (a deque, so pointers into it stay valid).
std::vector<const TemplateEntry*> SortMenuItems(
    v8::Isolate* isolate,
    const std::deque<TemplateEntry>& entries,
    std::deque<TemplateEntry>* synthesized) {
  std::vector<const TemplateEntry*> separators;
  std::vector<Group> groups(1);
  for (const TemplateEntry& e : entries) {
    if (e.is_plain_separator()) {
      separators.push_back(&e);
      if (!groups.back().empty())
        groups.emplace_back();
    } else {
      groups.back().push_back(&e);
    }
  }
  if (groups.back().empty())
    groups.pop_back();

  while (AttemptToMergeAGroup(groups)) {
  }
  for (Group& group : groups)
    group = SortItemsInGroup(group);
  groups = SortGroups(groups);

  std::vector<const TemplateEntry*> joined;
  size_t next_separator = 0;
  for (size_t i = 0; i < groups.size(); ++i) {
    if (i > 0 && !groups[i].empty()) {
      if (next_separator < separators.size()) {
        joined.push_back(separators[next_separator++]);
      } else {
        gin_helper::Dictionary sep = gin::Dictionary::CreateEmpty(isolate);
        sep.Set("type", std::string_view("separator"));
        TemplateEntry e;
        e.value = gin::ConvertToV8(isolate, sep).As<v8::Object>();
        e.separator = true;
        synthesized->push_back(std::move(e));
        joined.push_back(&synthesized->back());
      }
    }
    joined.insert(joined.end(), groups[i].begin(), groups[i].end());
  }
  return joined;
}

std::vector<const TemplateEntry*> RemoveExtraSeparators(
    std::vector<const TemplateEntry*> items) {
  // Fold adjacent separators together.
  std::vector<const TemplateEntry*> folded;
  for (size_t i = 0; i < items.size(); ++i) {
    const TemplateEntry* e = items[i];
    bool keep =
        e->hidden || !e->separator || i == 0 || !items[i - 1]->separator;
    if (keep)
      folded.push_back(e);
  }
  // Remove edge separators.
  std::vector<const TemplateEntry*> out;
  for (size_t i = 0; i < folded.size(); ++i) {
    const TemplateEntry* e = folded[i];
    bool keep =
        e->hidden || !e->separator || (i != 0 && i != folded.size() - 1);
    if (keep)
      out.push_back(e);
  }
  return out;
}

}  // namespace

// static
v8::Local<v8::Value> Menu::SortTemplate(v8::Isolate* isolate,
                                        v8::Local<v8::Value> tmpl) {
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (!IsArray(tmpl))
    return tmpl;
  v8::Local<v8::Object> array = tmpl.As<v8::Object>();
  const uint32_t length = ArrayLength(isolate, array);

  std::deque<TemplateEntry> entries;
  for (uint32_t i = 0; i < length; ++i) {
    v8::Local<v8::Value> value;
    if (array->HasOwnProperty(context, i).FromMaybe(false) &&
        array->Get(context, i).ToLocal(&value) && value->IsObject()) {
      entries.push_back(ReadEntry(isolate, value.As<v8::Object>()));
    }
  }

  std::deque<TemplateEntry> synthesized;
  std::vector<const TemplateEntry*> ordered =
      RemoveExtraSeparators(SortMenuItems(isolate, entries, &synthesized));

  v8::LocalVector<v8::Value> out(isolate);
  for (const TemplateEntry* e : ordered)
    out.push_back(e->value);
  return v8::Array::New(isolate, out.data(), out.size());
}

}  // namespace electron::api
