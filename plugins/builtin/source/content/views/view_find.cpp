#include "content/views/view_find.hpp"

#include <hex/api/imhex_api.hpp>
#include <hex/api/achievement_manager.hpp>

#include <hex/providers/buffered_reader.hpp>

#include <array>
#include <regex>
#include <string>
#include <utility>
#include <charconv>

#include <llvm/Demangle/Demangle.h>

namespace hex::plugin::builtin {

    ViewFind::ViewFind() : View("hex.builtin.view.find.name") {
        const static auto HighlightColor = [] { return (ImGui::GetCustomColorU32(ImGuiCustomCol_ToolbarPurple) & 0x00FFFFFF) | 0x70000000; };

        ImHexApi::HexEditor::addBackgroundHighlightingProvider([this](u64 address, const u8* data, size_t size, bool) -> std::optional<color_t> {
            hex::unused(data, size);

            if (this->m_searchTask.isRunning())
                return { };

            if (!this->m_occurrenceTree->overlapping({ address, address + size }).empty())
                return HighlightColor();
            else
                return std::nullopt;
        });

        ImHexApi::HexEditor::addTooltipProvider([this](u64 address, const u8* data, size_t size) {
            hex::unused(data, size);

            if (this->m_searchTask.isRunning())
                return;

            auto occurrences = this->m_occurrenceTree->overlapping({ address, address + size });
            if (occurrences.empty())
                return;

            ImGui::BeginTooltip();

            for (const auto &occurrence : occurrences) {
                ImGui::PushID(&occurrence);
                if (ImGui::BeginTable("##tooltips", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_NoClip)) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();

                    {
                        auto region = occurrence.value.region;
                        const auto value = this->decodeValue(ImHexApi::Provider::get(), occurrence.value, 256);

                        ImGui::ColorButton("##color", ImColor(HighlightColor()));
                        ImGui::SameLine(0, 10);
                        ImGui::TextFormatted("{} ", value);

                        if (ImGui::GetIO().KeyShift) {
                            ImGui::Indent();
                            if (ImGui::BeginTable("##extra_info", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_NoClip)) {

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextFormatted("{}: ", "hex.builtin.common.region"_lang);
                                ImGui::TableNextColumn();
                                ImGui::TextFormatted("[ 0x{:08X} - 0x{:08X} ]", region.getStartAddress(), region.getEndAddress());

                                auto demangledValue = llvm::demangle(value);

                                if (value != demangledValue) {
                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::TextFormatted("{}: ", "hex.builtin.view.find.demangled"_lang);
                                    ImGui::TableNextColumn();
                                    ImGui::TextFormatted("{}", demangledValue);
                                }

                                ImGui::EndTable();
                            }
                            ImGui::Unindent();
                        }
                    }


                    ImGui::PushStyleColor(ImGuiCol_TableRowBg, HighlightColor());
                    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, HighlightColor());
                    ImGui::EndTable();
                    ImGui::PopStyleColor(2);
                }
                ImGui::PopID();
            }

            ImGui::EndTooltip();
        });

        ShortcutManager::addShortcut(this, CTRLCMD + Keys::A, [this] {
            if (this->m_filterTask.isRunning())
                return;
            if (this->m_searchTask.isRunning())
                return;

            for (auto &occurrence : *this->m_sortedOccurrences)
                occurrence.selected = true;
        });
    }

    template<typename Type, typename StorageType>
    static std::tuple<bool, std::variant<u64, i64, float, double>, size_t> parseNumericValue(const std::string &string) {
        static_assert(sizeof(StorageType) >= sizeof(Type));

        StorageType value;

        std::size_t processed = 0;
        try {
            if constexpr (std::floating_point<Type>)
                value = std::stod(string, &processed);
            else if constexpr (std::signed_integral<Type>)
                value = std::stoll(string, &processed, 0);
            else
                value = std::stoull(string, &processed, 0);
        } catch (std::exception &) {
            return { false, { }, 0 };
        }

        if (processed != string.size())
            return { false, { }, 0 };

        if (value < std::numeric_limits<Type>::lowest() || value > std::numeric_limits<Type>::max())
            return { false, { }, 0 };

        return { true, value, sizeof(Type) };
    }

    std::tuple<bool, std::variant<u64, i64, float, double>, size_t> ViewFind::parseNumericValueInput(const std::string &input, SearchSettings::Value::Type type) {
        switch (type) {
            using enum SearchSettings::Value::Type;

            case U8:    return parseNumericValue<u8,  u64>(input);
            case U16:   return parseNumericValue<u16, u64>(input);
            case U32:   return parseNumericValue<u32, u64>(input);
            case U64:   return parseNumericValue<u64, u64>(input);
            case I8:    return parseNumericValue<i8,  i64>(input);
            case I16:   return parseNumericValue<i16, i64>(input);
            case I32:   return parseNumericValue<i32, i64>(input);
            case I64:   return parseNumericValue<i64, i64>(input);
            case F32:   return parseNumericValue<float, float>(input);
            case F64:   return parseNumericValue<double, double>(input);
            default:    return { false, { }, 0 };
        }
    }

    template<typename T>
    static std::string formatBytes(const std::vector<u8> &bytes) {
        if (bytes.size() > sizeof(T))
            return { };

        T value = 0x00;
        std::memcpy(&value, bytes.data(), bytes.size());

        if (std::signed_integral<T>)
            value = hex::signExtend(bytes.size() * 8, value);

        return hex::format("{}", value);
    }

    std::vector<ViewFind::Occurrence> ViewFind::searchStrings(Task &task, prv::Provider *provider, hex::Region searchRegion, const SearchSettings::Strings &settings) {
        using enum SearchSettings::StringType;

        std::vector<Occurrence> results;

        if (settings.type == ASCII_UTF16BE || settings.type == ASCII_UTF16LE) {
            auto newSettings = settings;

            newSettings.type = ASCII;
            auto asciiResults = searchStrings(task, provider, searchRegion, newSettings);
            std::copy(asciiResults.begin(), asciiResults.end(), std::back_inserter(results));

            if (settings.type == ASCII_UTF16BE) {
                newSettings.type = UTF16BE;
                auto utf16Results = searchStrings(task, provider, searchRegion, newSettings);
                std::copy(utf16Results.begin(), utf16Results.end(), std::back_inserter(results));
            } else if (settings.type == ASCII_UTF16LE) {
                newSettings.type = UTF16LE;
                auto utf16Results = searchStrings(task, provider, searchRegion, newSettings);
                std::copy(utf16Results.begin(), utf16Results.end(), std::back_inserter(results));
            }

            return results;
        }

        auto reader = prv::ProviderReader(provider);
        reader.seek(searchRegion.getStartAddress());
        reader.setEndAddress(searchRegion.getEndAddress());

        const auto [decodeType, endian] = [&] -> std::pair<Occurrence::DecodeType, std::endian> {
            if (settings.type == ASCII)
                return { Occurrence::DecodeType::ASCII, std::endian::native };
            else if (settings.type == SearchSettings::StringType::UTF16BE)
                return { Occurrence::DecodeType::UTF16, std::endian::big };
            else if (settings.type == SearchSettings::StringType::UTF16LE)
                return { Occurrence::DecodeType::UTF16, std::endian::little };
            else
                return { Occurrence::DecodeType::Binary, std::endian::native };
        }();

        size_t countedCharacters = 0;
        u64 startAddress = reader.begin().getAddress();
        u64 endAddress = reader.end().getAddress();

        u64 progress = 0;
        for (u8 byte : reader) {
            bool validChar =
                (settings.lowerCaseLetters    && std::islower(byte))  ||
                (settings.upperCaseLetters    && std::isupper(byte))  ||
                (settings.numbers             && std::isdigit(byte))  ||
                (settings.spaces              && std::isspace(byte) && byte != '\r' && byte != '\n')  ||
                (settings.underscores         && byte == '_')             ||
                (settings.symbols             && std::ispunct(byte) && !std::isspace(byte))  ||
                (settings.lineFeeds           && (byte == '\r' || byte == '\n'));

            if (settings.type == UTF16LE) {
                // Check if second byte of UTF-16 encoded string is 0x00
                if (countedCharacters % 2 == 1)
                    validChar = byte == 0x00;
            } else if (settings.type == UTF16BE) {
                // Check if first byte of UTF-16 encoded string is 0x00
                if (countedCharacters % 2 == 0)
                    validChar = byte == 0x00;
            }

            task.update(progress);

            if (validChar)
                countedCharacters++;
            if (!validChar || startAddress + countedCharacters == endAddress) {
                if (countedCharacters >= size_t(settings.minLength)) {
                    if (!settings.nullTermination || byte == 0x00) {
                        results.push_back(Occurrence { Region { startAddress, countedCharacters }, decodeType, endian, false });
                    }
                }

                startAddress += countedCharacters + 1;
                countedCharacters = 0;
                progress = startAddress - searchRegion.getStartAddress();

            }
        }

        return results;
    }

    std::vector<ViewFind::Occurrence> ViewFind::searchSequence(Task &task, prv::Provider *provider, hex::Region searchRegion, const SearchSettings::Sequence &settings) {
        std::vector<Occurrence> results;

        auto reader = prv::ProviderReader(provider);
        reader.seek(searchRegion.getStartAddress());
        reader.setEndAddress(searchRegion.getEndAddress());

        auto bytes = hex::decodeByteString(settings.sequence);

        if (bytes.empty())
            return { };

        auto occurrence = reader.begin();
        u64 progress = 0;
        while (true) {
            task.update(progress);

            occurrence = std::search(reader.begin(), reader.end(), std::boyer_moore_horspool_searcher(bytes.begin(), bytes.end()));
            if (occurrence == reader.end())
                break;

            auto address = occurrence.getAddress();
            reader.seek(address + 1);
            results.push_back(Occurrence{ Region { address, bytes.size() }, Occurrence::DecodeType::Binary, std::endian::native, false });
            progress = address - searchRegion.getStartAddress();
        }

        return results;
    }

    std::vector<ViewFind::Occurrence> ViewFind::searchRegex(Task &task, prv::Provider *provider, hex::Region searchRegion, const SearchSettings::Regex &settings) {
        auto stringOccurrences = searchStrings(task, provider, searchRegion, SearchSettings::Strings {
            .minLength          = settings.minLength,
            .nullTermination    = settings.nullTermination,
            .type               = settings.type,
            .lowerCaseLetters   = true,
            .upperCaseLetters   = true,
            .numbers            = true,
            .underscores        = true,
            .symbols            = true,
            .spaces             = true,
            .lineFeeds          = true
        });

        std::vector<Occurrence> result;
        std::regex regex(settings.pattern);
        for (const auto &occurrence : stringOccurrences) {
            std::string string(occurrence.region.getSize(), '\x00');
            provider->read(occurrence.region.getStartAddress(), string.data(), occurrence.region.getSize());

            task.update();

            if (settings.fullMatch) {
                if (std::regex_match(string, regex))
                    result.push_back(occurrence);
            } else {
                if (std::regex_search(string, regex))
                    result.push_back(occurrence);
            }
        }

        return result;
    }

    std::vector<ViewFind::Occurrence> ViewFind::searchBinaryPattern(Task &task, prv::Provider *provider, hex::Region searchRegion, const SearchSettings::BinaryPattern &settings) {
        std::vector<Occurrence> results;

        auto reader = prv::ProviderReader(provider);
        reader.seek(searchRegion.getStartAddress());
        reader.setEndAddress(searchRegion.getEndAddress());

        const size_t patternSize = settings.pattern.getSize();

        if (settings.alignment == 1) {
            u32 matchedBytes = 0;
            for (auto it = reader.begin(); it < reader.end(); it += 1) {
                auto byte = *it;

                task.update(it.getAddress());
                if (settings.pattern.matchesByte(byte, matchedBytes)) {
                    matchedBytes++;
                    if (matchedBytes == settings.pattern.getSize()) {
                        auto occurrenceAddress = it.getAddress() - (patternSize - 1);

                        results.push_back(Occurrence { Region { occurrenceAddress, patternSize }, Occurrence::DecodeType::Binary, std::endian::native, false });
                        it.setAddress(occurrenceAddress);
                        matchedBytes = 0;
                    }
                } else {
                    if (matchedBytes > 0)
                        it -= matchedBytes;
                    matchedBytes = 0;
                }
            }
        } else {
            std::vector<u8> data(patternSize);
            for (u64 address = searchRegion.getStartAddress(); address < searchRegion.getEndAddress(); address += settings.alignment) {
                reader.read(address, data.data(), data.size());

                task.update(address);

                bool match = true;
                for (u32 i = 0; i < patternSize; i++) {
                    if (settings.pattern.matchesByte(data[i], i)) {
                        match = false;
                        break;
                    }
                }

                if (match)
                    results.push_back(Occurrence { Region { address, patternSize }, Occurrence::DecodeType::Binary, std::endian::native, false });
            }
        }

        return results;
    }

    std::vector<ViewFind::Occurrence> ViewFind::searchValue(Task &task, prv::Provider *provider, Region searchRegion, const SearchSettings::Value &settings) {
        std::vector<Occurrence> results;

        auto reader = prv::ProviderReader(provider);
        reader.seek(searchRegion.getStartAddress());
        reader.setEndAddress(searchRegion.getEndAddress());

        auto inputMin = settings.inputMin;
        auto inputMax = settings.inputMax;

        if (inputMax.empty())
            inputMax = inputMin;

        const auto [validMin, min, sizeMin] = parseNumericValueInput(inputMin, settings.type);
        const auto [validMax, max, sizeMax] = parseNumericValueInput(inputMax, settings.type);

        if (!validMin || !validMax || sizeMin != sizeMax)
            return { };

        const auto size = sizeMin;

        const auto advance = settings.aligned ? size : 1;

        for (u64 address = searchRegion.getStartAddress(); address < searchRegion.getEndAddress(); address += advance) {
            task.update(address);

            auto result = std::visit([&](auto tag) {
                using T = std::remove_cvref_t<std::decay_t<decltype(tag)>>;

                auto minValue = std::get<T>(min);
                auto maxValue = std::get<T>(max);

                T value = 0;
                reader.read(address, reinterpret_cast<u8*>(&value), size);
                value = hex::changeEndianess(value, size, settings.endian);

                return value >= minValue && value <= maxValue;
            }, min);

            if (result) {
                Occurrence::DecodeType decodeType = [&]{
                    switch (settings.type) {
                        using enum SearchSettings::Value::Type;
                        using enum Occurrence::DecodeType;

                        case U8:
                        case U16:
                        case U32:
                        case U64:
                            return Unsigned;
                        case I8:
                        case I16:
                        case I32:
                        case I64:
                            return Signed;
                        case F32:
                            return Float;
                        case F64:
                            return Double;
                        default:
                            return Binary;
                    }
                }();

                results.push_back(Occurrence { Region { address, size }, decodeType, settings.endian, false });
            }
        }

        return results;
    }

    void ViewFind::runSearch() {
        Region searchRegion = this->m_searchSettings.region;

        if (this->m_searchSettings.mode == SearchSettings::Mode::Strings)
            AchievementManager::unlockAchievement("hex.builtin.achievement.find", "hex.builtin.achievement.find.find_strings.name");
        else if (this->m_searchSettings.mode == SearchSettings::Mode::Sequence)
            AchievementManager::unlockAchievement("hex.builtin.achievement.find", "hex.builtin.achievement.find.find_specific_string.name");
        else if (this->m_searchSettings.mode == SearchSettings::Mode::Value) {
            if (this->m_searchSettings.value.inputMin == "250" && this->m_searchSettings.value.inputMax == "1000")
                AchievementManager::unlockAchievement("hex.builtin.achievement.find", "hex.builtin.achievement.find.find_specific_string.name");
        }

        this->m_occurrenceTree->clear();

        this->m_searchTask = TaskManager::createTask("hex.builtin.view.find.searching", searchRegion.getSize(), [this, settings = this->m_searchSettings, searchRegion](auto &task) {
            auto provider = ImHexApi::Provider::get();

            switch (settings.mode) {
                using enum SearchSettings::Mode;
                case Strings:
                    this->m_foundOccurrences.get(provider) = searchStrings(task, provider, searchRegion, settings.strings);
                    break;
                case Sequence:
                    this->m_foundOccurrences.get(provider) = searchSequence(task, provider, searchRegion, settings.bytes);
                    break;
                case Regex:
                    this->m_foundOccurrences.get(provider) = searchRegex(task, provider, searchRegion, settings.regex);
                    break;
                case BinaryPattern:
                    this->m_foundOccurrences.get(provider) = searchBinaryPattern(task, provider, searchRegion, settings.binaryPattern);
                    break;
                case Value:
                    this->m_foundOccurrences.get(provider) = searchValue(task, provider, searchRegion, settings.value);
                    break;
            }

            this->m_sortedOccurrences.get(provider) = this->m_foundOccurrences.get(provider);

            for (const auto &occurrence : this->m_foundOccurrences.get(provider))
                this->m_occurrenceTree->insert({ occurrence.region.getStartAddress(), occurrence.region.getEndAddress() }, occurrence);
        });
    }

    std::string ViewFind::decodeValue(prv::Provider *provider, Occurrence occurrence, size_t maxBytes) const {
        std::vector<u8> bytes(std::min<size_t>(occurrence.region.getSize(), maxBytes));
        provider->read(occurrence.region.getStartAddress(), bytes.data(), bytes.size());

        std::string result;
        switch (this->m_decodeSettings.mode) {
            using enum SearchSettings::Mode;

            case Value:
            case Strings:
            {
                switch (occurrence.decodeType) {
                    using enum Occurrence::DecodeType;
                    case Binary:
                    case ASCII:
                        result = hex::encodeByteString(bytes);
                        break;
                    case UTF16:
                        for (size_t i = occurrence.endian == std::endian::little ? 0 : 1; i < bytes.size(); i += 2)
                            result += hex::encodeByteString({ bytes[i] });
                        break;
                    case Unsigned:
                        result += formatBytes<u64>(bytes);
                        break;
                    case Signed:
                        result += formatBytes<i64>(bytes);
                        break;
                    case Float:
                        result += formatBytes<float>(bytes);
                        break;
                    case Double:
                        result += formatBytes<double>(bytes);
                        break;
                }
            }
                break;
            case Sequence:
            case Regex:
            case BinaryPattern:
                result = hex::encodeByteString(bytes);
                break;
        }

        if (occurrence.region.getSize() > maxBytes)
            result += "...";

        return result;
    }

    void ViewFind::drawContextMenu(Occurrence &target, const std::string &value) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsItemHovered()) {
            ImGui::OpenPopup("FindContextMenu");
            target.selected = true;
            this->m_replaceBuffer.clear();
        }

        if (ImGui::BeginPopup("FindContextMenu")) {
            if (ImGui::MenuItem("hex.builtin.view.find.context.copy"_lang))
                ImGui::SetClipboardText(value.c_str());
            if (ImGui::MenuItem("hex.builtin.view.find.context.copy_demangle"_lang))
                ImGui::SetClipboardText(llvm::demangle(value).c_str());
            if (ImGui::BeginMenu("hex.builtin.view.find.context.replace"_lang)) {
                if (ImGui::BeginTabBar("##replace_tabs")) {
                    if (ImGui::BeginTabItem("hex.builtin.view.find.context.replace.hex"_lang)) {
                        ImGui::InputTextIcon("##replace_input", ICON_VS_SYMBOL_NAMESPACE, this->m_replaceBuffer);

                        ImGui::BeginDisabled(this->m_replaceBuffer.empty());
                        if (ImGui::Button("hex.builtin.view.find.context.replace"_lang)) {
                            auto provider = ImHexApi::Provider::get();
                            auto bytes = parseHexString(this->m_replaceBuffer);

                            for (const auto &occurrence : *this->m_sortedOccurrences) {
                                if (occurrence.selected) {
                                    size_t size = std::min<size_t>(occurrence.region.size, bytes.size());
                                    provider->write(occurrence.region.getStartAddress(), bytes.data(), size);
                                }
                            }
                        }
                        ImGui::EndDisabled();

                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("hex.builtin.view.find.context.replace.ascii"_lang)) {
                        ImGui::InputTextIcon("##replace_input", ICON_VS_SYMBOL_KEY, this->m_replaceBuffer);

                        ImGui::BeginDisabled(this->m_replaceBuffer.empty());
                        if (ImGui::Button("hex.builtin.view.find.context.replace"_lang)) {
                            auto provider = ImHexApi::Provider::get();
                            auto bytes = decodeByteString(this->m_replaceBuffer);

                            for (const auto &occurrence : *this->m_sortedOccurrences) {
                                if (occurrence.selected) {
                                    size_t size = std::min<size_t>(occurrence.region.size, bytes.size());
                                    provider->write(occurrence.region.getStartAddress(), bytes.data(), size);
                                }
                            }
                        }
                        ImGui::EndDisabled();

                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                ImGui::EndMenu();
            }

            ImGui::EndPopup();
        }
    }

    void ViewFind::drawContent() {
        if (ImGui::Begin(View::toWindowName("hex.builtin.view.find.name").c_str(), &this->getWindowOpenState())) {
            auto provider = ImHexApi::Provider::get();

            ImGui::BeginDisabled(this->m_searchTask.isRunning());
            {
                ui::regionSelectionPicker(&this->m_searchSettings.region, provider, &this->m_searchSettings.range, true, true);

                ImGui::NewLine();

                if (ImGui::BeginTabBar("SearchMethods")) {
                    const std::array<std::string, 5> StringTypes = {
                            "hex.builtin.common.encoding.ascii"_lang,
                            "hex.builtin.common.encoding.utf16le"_lang,
                            "hex.builtin.common.encoding.utf16be"_lang,
                            hex::format("{} + {}", "hex.builtin.common.encoding.ascii"_lang, "hex.builtin.common.encoding.utf16le"_lang),
                            hex::format("{} + {}", "hex.builtin.common.encoding.ascii"_lang, "hex.builtin.common.encoding.utf16be"_lang)
                    };

                    auto &mode = this->m_searchSettings.mode;
                    if (ImGui::BeginTabItem("hex.builtin.view.find.strings"_lang)) {
                        auto &settings = this->m_searchSettings.strings;
                        mode = SearchSettings::Mode::Strings;

                        ImGui::InputInt("hex.builtin.view.find.strings.min_length"_lang, &settings.minLength, 1, 1);
                        if (settings.minLength < 1)
                            settings.minLength = 1;

                        if (ImGui::BeginCombo("hex.builtin.common.type"_lang, StringTypes[std::to_underlying(settings.type)].c_str())) {
                            for (size_t i = 0; i < StringTypes.size(); i++) {
                                auto type = static_cast<SearchSettings::StringType>(i);

                                if (ImGui::Selectable(StringTypes[i].c_str(), type == settings.type))
                                    settings.type = type;
                            }
                            ImGui::EndCombo();
                        }

                        if (ImGui::CollapsingHeader("hex.builtin.view.find.strings.match_settings"_lang)) {
                            ImGui::Checkbox("hex.builtin.view.find.strings.null_term"_lang, &settings.nullTermination);

                            ImGui::Header("hex.builtin.view.find.strings.chars"_lang);
                            ImGui::Checkbox(hex::format("{} [a-z]", "hex.builtin.view.find.strings.lower_case"_lang.get()).c_str(), &settings.lowerCaseLetters);
                            ImGui::Checkbox(hex::format("{} [A-Z]", "hex.builtin.view.find.strings.upper_case"_lang.get()).c_str(), &settings.upperCaseLetters);
                            ImGui::Checkbox(hex::format("{} [0-9]", "hex.builtin.view.find.strings.numbers"_lang.get()).c_str(), &settings.numbers);
                            ImGui::Checkbox(hex::format("{} [_]", "hex.builtin.view.find.strings.underscores"_lang.get()).c_str(), &settings.underscores);
                            ImGui::Checkbox(hex::format("{} [!\"#$%...]", "hex.builtin.view.find.strings.symbols"_lang.get()).c_str(), &settings.symbols);
                            ImGui::Checkbox(hex::format("{} [ \\f\\t\\v]", "hex.builtin.view.find.strings.spaces"_lang.get()).c_str(), &settings.spaces);
                            ImGui::Checkbox(hex::format("{} [\\r\\n]", "hex.builtin.view.find.strings.line_feeds"_lang.get()).c_str(), &settings.lineFeeds);
                        }

                        this->m_settingsValid = true;

                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("hex.builtin.view.find.sequences"_lang)) {
                        auto &settings = this->m_searchSettings.bytes;

                        mode = SearchSettings::Mode::Sequence;

                        ImGui::InputTextIcon("hex.builtin.common.value"_lang, ICON_VS_SYMBOL_KEY, settings.sequence);

                        this->m_settingsValid = !settings.sequence.empty() && !hex::decodeByteString(settings.sequence).empty();

                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("hex.builtin.view.find.regex"_lang)) {
                        auto &settings = this->m_searchSettings.regex;

                        mode = SearchSettings::Mode::Regex;

                        ImGui::InputInt("hex.builtin.view.find.strings.min_length"_lang, &settings.minLength, 1, 1);
                        if (settings.minLength < 1)
                            settings.minLength = 1;

                        if (ImGui::BeginCombo("hex.builtin.common.type"_lang, StringTypes[std::to_underlying(settings.type)].c_str())) {
                            for (size_t i = 0; i < StringTypes.size(); i++) {
                                auto type = static_cast<SearchSettings::StringType>(i);

                                if (ImGui::Selectable(StringTypes[i].c_str(), type == settings.type))
                                    settings.type = type;
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::Checkbox("hex.builtin.view.find.strings.null_term"_lang, &settings.nullTermination);

                        ImGui::NewLine();

                        ImGui::InputTextIcon("hex.builtin.view.find.regex.pattern"_lang, ICON_VS_REGEX, settings.pattern);

                        try {
                            std::regex regex(settings.pattern);
                            this->m_settingsValid = true;
                        } catch (std::regex_error &e) {
                            this->m_settingsValid = false;
                        }

                        if (settings.pattern.empty())
                            this->m_settingsValid = false;

                        ImGui::Checkbox("hex.builtin.view.find.regex.full_match"_lang, &settings.fullMatch);

                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("hex.builtin.view.find.binary_pattern"_lang)) {
                        auto &settings = this->m_searchSettings.binaryPattern;

                        mode = SearchSettings::Mode::BinaryPattern;

                        ImGui::InputTextIcon("hex.builtin.view.find.binary_pattern"_lang, ICON_VS_SYMBOL_NAMESPACE, settings.input);

                        constexpr static u32 min = 1, max = 0x1000;
                        ImGui::SliderScalar("hex.builtin.view.find.binary_pattern.alignment"_lang, ImGuiDataType_U32, &settings.alignment, &min, &max);

                        settings.pattern = hex::BinaryPattern(settings.input);
                        this->m_settingsValid = settings.pattern.isValid() && settings.alignment > 0;

                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("hex.builtin.view.find.value"_lang)) {
                        auto &settings = this->m_searchSettings.value;

                        mode = SearchSettings::Mode::Value;

                        bool edited = false;

                        if (settings.range) {
                            if (ImGui::InputTextIcon("hex.builtin.view.find.value.min"_lang, ICON_VS_SYMBOL_NUMERIC, settings.inputMin)) edited = true;
                            if (ImGui::InputTextIcon("hex.builtin.view.find.value.max"_lang, ICON_VS_SYMBOL_NUMERIC, settings.inputMax)) edited = true;
                        } else {
                            if (ImGui::InputTextIcon("hex.builtin.common.value"_lang, ICON_VS_SYMBOL_NUMERIC, settings.inputMin)) {
                                edited = true;
                                settings.inputMax = settings.inputMin;
                            }

                            ImGui::BeginDisabled();
                            ImGui::InputTextIcon("##placeholder_value", ICON_VS_SYMBOL_NUMERIC, settings.inputMax);
                            ImGui::EndDisabled();
                        }

                        ImGui::Checkbox("hex.builtin.view.find.value.range"_lang, &settings.range);
                        ImGui::NewLine();

                        const std::array<std::string, 10> InputTypes = {
                                "hex.builtin.common.type.u8"_lang,
                                "hex.builtin.common.type.u16"_lang,
                                "hex.builtin.common.type.u32"_lang,
                                "hex.builtin.common.type.u64"_lang,
                                "hex.builtin.common.type.i8"_lang,
                                "hex.builtin.common.type.i16"_lang,
                                "hex.builtin.common.type.i32"_lang,
                                "hex.builtin.common.type.i64"_lang,
                                "hex.builtin.common.type.f32"_lang,
                                "hex.builtin.common.type.f64"_lang
                        };

                        if (ImGui::BeginCombo("hex.builtin.common.type"_lang, InputTypes[std::to_underlying(settings.type)].c_str())) {
                            for (size_t i = 0; i < InputTypes.size(); i++) {
                                auto type = static_cast<SearchSettings::Value::Type>(i);

                                if (ImGui::Selectable(InputTypes[i].c_str(), type == settings.type)) {
                                    settings.type = type;
                                    edited = true;
                                }
                            }
                            ImGui::EndCombo();
                        }

                        {
                            int selection = [&] {
                                switch (settings.endian) {
                                    default:
                                    case std::endian::little:    return 0;
                                    case std::endian::big:       return 1;
                                }
                            }();

                            std::array options = { "hex.builtin.common.little"_lang, "hex.builtin.common.big"_lang };
                            if (ImGui::SliderInt("hex.builtin.common.endian"_lang, &selection, 0, options.size() - 1, options[selection], ImGuiSliderFlags_NoInput)) {
                                edited = true;
                                switch (selection) {
                                    default:
                                    case 0: settings.endian = std::endian::little;   break;
                                    case 1: settings.endian = std::endian::big;      break;
                                }
                            }
                        }

                        ImGui::Checkbox("hex.builtin.view.find.value.aligned"_lang, &settings.aligned);

                        if (edited) {
                            auto [minValid, min, minSize] = parseNumericValueInput(settings.inputMin, settings.type);
                            auto [maxValid, max, maxSize] = parseNumericValueInput(settings.inputMax, settings.type);

                            this->m_settingsValid = minValid && maxValid && minSize == maxSize;
                        }

                        if (settings.inputMin.empty())
                            this->m_settingsValid = false;

                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                ImGui::NewLine();

                ImGui::BeginDisabled(!this->m_settingsValid);
                {
                    if (ImGui::Button("hex.builtin.view.find.search"_lang)) {
                        this->runSearch();

                        this->m_decodeSettings = this->m_searchSettings;
                    }
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::TextFormatted("hex.builtin.view.find.search.entries"_lang, this->m_foundOccurrences->size());

                ImGui::BeginDisabled(this->m_foundOccurrences->empty());
                {
                    if (ImGui::Button("hex.builtin.view.find.search.reset"_lang)) {
                        this->m_foundOccurrences->clear();
                        this->m_sortedOccurrences->clear();
                        *this->m_occurrenceTree = {};
                    }
                }
                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();


            ImGui::Separator();
            ImGui::NewLine();

            auto &currOccurrences = *this->m_sortedOccurrences;

            ImGui::PushItemWidth(-1);
            auto prevFilterLength = this->m_currFilter->length();
            if (ImGui::InputTextIcon("##filter", ICON_VS_FILTER, *this->m_currFilter)) {
                if (prevFilterLength > this->m_currFilter->length())
                    *this->m_sortedOccurrences = *this->m_foundOccurrences;

                if (this->m_filterTask.isRunning())
                    this->m_filterTask.interrupt();

                if (!this->m_currFilter->empty()) {
                    this->m_filterTask = TaskManager::createTask("Filtering", currOccurrences.size(), [this, provider, &currOccurrences](Task &task) {
                        u64 progress = 0;
                        currOccurrences.erase(std::remove_if(currOccurrences.begin(), currOccurrences.end(), [this, provider, &task, &progress](const auto &region) {
                            task.update(progress);
                            progress += 1;

                            return !hex::containsIgnoreCase(this->decodeValue(provider, region), this->m_currFilter.get(provider));
                        }), currOccurrences.end());
                    });
                }
            }
            ImGui::PopItemWidth();

            if (ImGui::BeginTable("##entries", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("hex.builtin.common.offset"_lang, 0, -1, ImGui::GetID("offset"));
                ImGui::TableSetupColumn("hex.builtin.common.size"_lang, 0, -1, ImGui::GetID("size"));
                ImGui::TableSetupColumn("hex.builtin.common.value"_lang, 0, -1, ImGui::GetID("value"));

                auto sortSpecs = ImGui::TableGetSortSpecs();

                if (sortSpecs->SpecsDirty) {
                    std::sort(currOccurrences.begin(), currOccurrences.end(), [this, &sortSpecs, provider](Occurrence &left, Occurrence &right) -> bool {
                        if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("offset")) {
                            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                                return left.region.getStartAddress() > right.region.getStartAddress();
                            else
                                return left.region.getStartAddress() < right.region.getStartAddress();
                        } else if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("size")) {
                            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                                return left.region.getSize() > right.region.getSize();
                            else
                                return left.region.getSize() < right.region.getSize();
                        } else if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("value")) {
                            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                                return this->decodeValue(provider, left) > this->decodeValue(provider, right);
                            else
                                return this->decodeValue(provider, left) < this->decodeValue(provider, right);
                        }

                        return false;
                    });

                    sortSpecs->SpecsDirty = false;
                }

                ImGui::TableHeadersRow();

                ImGuiListClipper clipper;
                clipper.Begin(currOccurrences.size(), ImGui::GetTextLineHeightWithSpacing());

                while (clipper.Step()) {
                    for (size_t i = clipper.DisplayStart; i < std::min<size_t>(clipper.DisplayEnd, currOccurrences.size()); i++) {
                        auto &foundItem = currOccurrences[i];

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();

                        ImGui::TextFormatted("0x{:08X}", foundItem.region.getStartAddress());
                        ImGui::TableNextColumn();
                        ImGui::TextFormatted("{}", hex::toByteString(foundItem.region.getSize()));
                        ImGui::TableNextColumn();

                        ImGui::PushID(i);

                        auto value = this->decodeValue(provider, foundItem, 256);
                        ImGui::TextFormatted("{}", value);
                        ImGui::SameLine();
                        if (ImGui::Selectable("##line", foundItem.selected, ImGuiSelectableFlags_SpanAllColumns)) {
                            if (ImGui::GetIO().KeyCtrl) {
                                foundItem.selected = !foundItem.selected;
                            } else {
                                for (auto &occurrence : *this->m_sortedOccurrences)
                                    occurrence.selected = false;
                                foundItem.selected = true;
                                ImHexApi::HexEditor::setSelection(foundItem.region.getStartAddress(), foundItem.region.getSize());
                            }
                        }
                        drawContextMenu(foundItem, value);

                        ImGui::PopID();
                    }
                }
                clipper.End();

                ImGui::EndTable();
            }

        }
        ImGui::End();
    }

}
