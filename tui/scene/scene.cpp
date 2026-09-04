// Scene layer (T2.4) — see scene.h.

#include "scene.h"

namespace npptui {

std::string SceneStatus::describe() const {
  std::string s = "status{file='" + fileName + "', dirty=" + (dirty ? "1" : "0") +
                  ", lang=" + language + ", ln=" + std::to_string(line) +
                  ", col=" + std::to_string(column);
  if (!encoding.empty()) s += ", enc=" + encoding;
  if (!transient.empty()) s += ", transient='" + transient + "'";
  return s + "}";
}

std::string ScenePrompt::describe() const {
  return "prompt{label='" + label + "', input='" + input + "'}";
}

std::string SceneTabs::describe() const {
  std::string s = "tabs{";
  for (size_t i = 0; i < tabs.size(); i++) {
    if (i) s += ", ";
    s += "'" + tabs[i].title + "'";
    if (tabs[i].dirty) s += "*";
    if (tabs[i].active) s += "!";
  }
  return s + "}";
}

}  // namespace npptui
