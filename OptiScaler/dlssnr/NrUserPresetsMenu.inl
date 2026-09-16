// Included inside namespace DlssNr. All settings use the same Config assignment path as the NR UI.
static void RenderUserPresets(Config& config)
{
    using namespace UserPresets;
    if (!ImGui::TreeNodeEx("Saved NR presets", ImGuiTreeNodeFlags_DefaultOpen)) return;
    static Json library;
    static std::string selected, message;
    static char name[128] {};
    static bool initialized = false, fileReady = false;
    const auto path = Util::ExePath().parent_path() / L"OptiScaler.NR-presets.json";
    auto refresh = [&]() {
        try { library = Read(path); fileReady = true; message.clear(); }
        catch (const std::exception& e) { fileReady = false; message = e.what(); }
        initialized = true;
    };
    if (!initialized) refresh();
    ImGui::SetNextItemWidth(250);
    if (ImGui::BeginCombo("##SavedNRPreset", selected.empty() ? "Choose a preset" : selected.c_str()))
    {
        if (fileReady)
            for (auto it = library["presets"].begin(); it != library["presets"].end(); ++it)
            {
                ImGui::PushID(it.key().c_str());
                if (ImGui::Selectable(it.key().c_str(), selected == it.key())) selected = it.key();
                ImGui::PopID();
            }
        ImGui::EndCombo();
    }
    const bool haveSelection = fileReady && !selected.empty() && library["presets"].contains(selected);
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveSelection);
    if (ImGui::Button("Load##NRPreset"))
    {
        try {
            auto fresh = Read(path);
            Apply(fresh.at("presets").at(selected), config);
            library = std::move(fresh);
            message = "Loaded " + selected + ". Model changes may briefly rebuild NR.";
        } catch (const std::exception& e) { message = std::string("Load failed: ") + e.what(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Replace##NRPreset")) ImGui::OpenPopup("Replace NR preset?");
    ImGui::SameLine();
    if (ImGui::Button("Delete##NRPreset")) ImGui::OpenPopup("Delete NR preset?");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh##NRPresets")) refresh();

    ImGui::SetNextItemWidth(250);
    ImGui::InputText("Name##NRPreset", name, sizeof(name));
    ImGui::SameLine();
    ImGui::BeginDisabled(!fileReady);
    if (ImGui::Button("Save new##NRPreset"))
    {
        try {
            const auto key = Name(name);
            auto fresh = Read(path);
            if (fresh["presets"].contains(key)) throw std::runtime_error("Name already exists; select it and use Replace");
            auto settings = Capture(config);
            Validate(settings, config);
            fresh["presets"][key] = std::move(settings);
            Write(path, fresh);
            library = std::move(fresh); selected = key;
            message = "Saved " + key; name[0] = 0;
        } catch (const std::exception& e) { message = std::string("Save failed: ") + e.what(); }
    }
    ImGui::EndDisabled();

    for (bool remove : {false, true})
    {
        const char* title = remove ? "Delete NR preset?" : "Replace NR preset?";
        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("%s '%s'%s", remove ? "Delete" : "Replace", selected.c_str(),
                               remove ? "?" : " with the current NR settings?");
            if (ImGui::Button(remove ? "Delete" : "Replace"))
            {
                try {
                    auto fresh = Read(path);
                    if (!fresh["presets"].contains(selected)) throw std::runtime_error("Preset no longer exists; refresh the list");
                    if (remove) fresh["presets"].erase(selected);
                    else { auto settings = Capture(config); Validate(settings, config); fresh["presets"][selected] = std::move(settings); }
                    Write(path, fresh);
                    library = std::move(fresh);
                    message = (remove ? "Deleted " : "Replaced ") + selected;
                    if (remove) selected.clear();
                } catch (const std::exception& e) { message = e.what(); }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::TextDisabled("Saved in OptiScaler.NR-presets.json beside the game.");
    ImGui::TextWrapped("Presets include all passes, resolution, processing mode and appearance. "
                       "Hotkeys, diagnostics and exposure calibration stay unchanged.");
    ImGui::TreePop();
}
