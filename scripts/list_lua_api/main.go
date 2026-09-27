package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

type Function struct {
	Name       string
	Module     string
	Documented bool
}

type Constant struct {
	Name       string
	Module     string
	Documented bool
}

type Module struct {
	Name      string
	DocFile   string
	Functions []Function
	Constants []Constant
}

var (
	outputPath       string
	undocumentedOnly bool
	showSummary      bool
)

var docFileMapping = map[string]string{
	"display":   "API-Display-and-Graphics.md",
	"input":     "API-Input.md",
	"sys":       "API-System-and-Config.md",
	"config":    "API-System-and-Config.md",
	"fs":        "API-Filesystem.md",
	"wifi":      "API-Network-and-WiFi.md",
	"network":   "API-Network-and-WiFi.md",
	"ui":        "API-UI.md",
	"audio":     "API-Audio-and-Sound.md",
	"sound":     "API-Audio-and-Sound.md",
	"perf":      "API-Performance.md",
	"graphics":  "API-Display-and-Graphics.md",
	"gfx3d":     "API-3D.md",
	"video":     "API-Video.md",
	"repl":      "API-Repl.md",
	"terminal":  "API-Terminal.md",
	"crypto":    "API-Crypto.md",
	"modplayer": "API-Modplayer.md",
	"sysconfig": "API-Sysconfig.md",
	"appconfig": "API-System-and-Config.md",
	"game":      "API-Game.md",
	"json":      "API-JSON.md",
	"tcp":       "API-TCP.md",
	"zip":       "API-Zip.md",
}

func main() {
	flag.StringVar(&outputPath, "output", "", "Output file path (default: stdout)")
	flag.BoolVar(&undocumentedOnly, "undocumented-only", false, "Show only undocumented items")
	flag.BoolVar(&showSummary, "summary", true, "Show summary counts")
	flag.Parse()

	modules := parseLuaBridges()
	docContents := readDocFiles()

	for i := range modules {
		checkDocumentation(&modules[i], docContents)
	}

	output(modules)
}

func parseLuaBridges() []Module {
	modules := make(map[string]*Module)

	entries, err := os.ReadDir("src/os")
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error reading src/os: %v\n", err)
		os.Exit(1)
	}

	for _, entry := range entries {
		if !entry.IsDir() && strings.HasPrefix(entry.Name(), "lua_bridge") && strings.HasSuffix(entry.Name(), ".c") {
			parseLuaBridgeFile(filepath.Join("src/os", entry.Name()), modules)
		}
	}

	result := make([]Module, 0, len(modules))
	for _, m := range modules {
		result = append(result, *m)
	}
	sort.Slice(result, func(i, j int) bool {
		return result[i].Name < result[j].Name
	})

	return result
}

func parseLuaBridgeFile(path string, modules map[string]*Module) {
	content, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error reading %s: %v\n", path, err)
		return
	}

	contentStr := string(content)

	moduleReg := regexp.MustCompile(`register_subtable\s*\(\s*L\s*,\s*"(\w+)"`)
	matches := moduleReg.FindAllStringSubmatch(contentStr, -1)

	// Modules built by hand and attached with lua_setfield(L, -2, "<name>")
	// (sys, graphics, network, game, tcp, zip) instead of register_subtable.
	// Only a name matching the file's own module (lua_bridge_<name>.c) or a
	// known module counts, so result-table fields ("name", "size") are skipped.
	fileModule := strings.TrimSuffix(strings.TrimPrefix(filepath.Base(path), "lua_bridge_"), ".c")
	setfieldReg := regexp.MustCompile(`lua_setfield\s*\(\s*L\s*,\s*-2\s*,\s*"(\w+)"\s*\)`)
	for _, match := range setfieldReg.FindAllStringSubmatch(contentStr, -1) {
		if _, known := docFileMapping[match[1]]; match[1] == fileModule || known {
			matches = append(matches, match)
		}
	}

	moduleNames := []string{}
	seenModules := map[string]bool{}
	for _, match := range matches {
		if len(match) > 1 {
			moduleName := match[1]
			if seenModules[moduleName] {
				continue
			}
			seenModules[moduleName] = true
			moduleNames = append(moduleNames, moduleName)
			if _, ok := modules[moduleName]; !ok {
				modules[moduleName] = &Module{
					Name:      moduleName,
					DocFile:   docFileMapping[moduleName],
					Functions: []Function{},
					Constants: []Constant{},
				}
			}
		}
	}

	// Sub-module files (lua_bridge_game_camera.c, _scene.c, _save.c) never
	// register a module themselves: lua_bridge_game.c attaches their tables
	// with lua_settable. Attribute their functions to the parent module,
	// qualified by the C prefix (l_camera_new -> "camera.new").
	if len(moduleNames) == 0 {
		parent, _, found := strings.Cut(fileModule, "_")
		if _, known := docFileMapping[parent]; !found || !known {
			return
		}
		if _, ok := modules[parent]; !ok {
			modules[parent] = &Module{Name: parent, DocFile: docFileMapping[parent]}
		}
		subReg := regexp.MustCompile(`\{\s*"(\w+)"\s*,\s*l_([a-z0-9]+)_\w+\s*\}`)
		for _, match := range subReg.FindAllStringSubmatch(contentStr, -1) {
			if strings.HasPrefix(match[1], "__") {
				continue
			}
			m := modules[parent]
			m.Functions = append(m.Functions, Function{Name: match[2] + "." + match[1], Module: parent})
		}
		return
	}

	fileConstReg := regexp.MustCompile(`lua_pushinteger\s*\(\s*L\s*,\s*(\w+)\s*\)\s*;\s*\n\s*lua_setfield\s*\(\s*L\s*,\s*-\s*2\s*,\s*"(\w+)"`)
	constMatches := fileConstReg.FindAllStringSubmatch(contentStr, -1)

	constBlacklist := map[string]bool{
		"nup": true, "name": true, "version": true, "description": true,
		"author": true, "requirements": true, "L": true, "lua": true,
	}

	for _, match := range constMatches {
		if len(match) > 2 {
			val := match[1]
			constName := match[2]
			if constBlacklist[constName] {
				continue
			}
			for _, modName := range moduleNames {
				if m, ok := modules[modName]; ok {
					m.Constants = append(m.Constants, Constant{Name: constName, Module: modName})
				}
			}
			_ = val
		}
	}

	funcReg := regexp.MustCompile(`\{\s*"(\w+)"\s*,\s*l_(\w+)_`)
	funcMatches := funcReg.FindAllStringSubmatch(contentStr, -1)

	for _, match := range funcMatches {
		if len(match) > 2 {
			funcName := match[1]
			possibleModule := match[2]
			for _, modName := range moduleNames {
				if m, ok := modules[modName]; ok {
					if possibleModule == modName {
						m.Functions = append(m.Functions, Function{Name: funcName, Module: modName})
					}
				}
			}
		}
	}

	funcReg2 := regexp.MustCompile(`\{\s*"(\w+)"\s*,\s*l_\w+\}`)
	funcMatches2 := funcReg2.FindAllStringSubmatch(contentStr, -1)

	for _, match := range funcMatches2 {
		if len(match) > 1 {
			funcName := match[1]
			exists := false
			for _, modName := range moduleNames {
				if m, ok := modules[modName]; ok {
					for _, f := range m.Functions {
						if f.Name == funcName {
							exists = true
							break
						}
					}
				}
			}
			if !exists {
				for _, modName := range moduleNames {
					if m, ok := modules[modName]; ok {
						m.Functions = append(m.Functions, Function{Name: funcName, Module: modName})
					}
				}
			}
		}
	}

	// Aliases: one luaL_Reg registered under two names (config and
	// appconfig both use l_config_lib) — the l_<module>_ match credits only
	// one of them, so a same-file module left empty gets its sibling's list.
	for _, modName := range moduleNames {
		if m := modules[modName]; m != nil && len(m.Functions) == 0 {
			for _, other := range moduleNames {
				if o := modules[other]; o != nil && other != modName && len(o.Functions) > 0 {
					for _, f := range o.Functions {
						m.Functions = append(m.Functions, Function{Name: f.Name, Module: modName})
					}
					break
				}
			}
		}
	}

	for _, modName := range moduleNames {
		if m, ok := modules[modName]; ok {
			seenFuncs := make(map[string]bool)
			uniqueFuncs := []Function{}
			for _, f := range m.Functions {
				if strings.HasPrefix(f.Name, "__") {
					continue // metamethods are not API
				}
				if !seenFuncs[f.Name] {
					seenFuncs[f.Name] = true
					uniqueFuncs = append(uniqueFuncs, f)
				}
			}
			m.Functions = uniqueFuncs

			seenConsts := make(map[string]bool)
			uniqueConsts := []Constant{}
			for _, c := range m.Constants {
				if !seenConsts[c.Name] {
					seenConsts[c.Name] = true
					uniqueConsts = append(uniqueConsts, c)
				}
			}
			m.Constants = uniqueConsts
		}
	}
}

func readDocFiles() map[string]string {
	contents := make(map[string]string)
	entries, err := os.ReadDir("docs")
	if err != nil {
		return contents
	}

	for _, entry := range entries {
		if !entry.IsDir() && strings.HasPrefix(entry.Name(), "API-") && strings.HasSuffix(entry.Name(), ".md") {
			path := filepath.Join("docs", entry.Name())
			content, err := os.ReadFile(path)
			if err == nil {
				contents[entry.Name()] = string(content)
			}
		}
	}
	return contents
}

func checkDocumentation(module *Module, docContents map[string]string) {
	if module.DocFile == "" {
		for i := range module.Functions {
			module.Functions[i].Documented = false
		}
		for i := range module.Constants {
			module.Constants[i].Documented = false
		}
		return
	}

	docContent, exists := docContents[module.DocFile]
	if !exists {
		for i := range module.Functions {
			module.Functions[i].Documented = false
		}
		for i := range module.Constants {
			module.Constants[i].Documented = false
		}
		return
	}

	for i := range module.Functions {
		fname := module.Functions[i].Name
		searchPatterns := []string{
			fmt.Sprintf("picocalc.%s.%s(", module.Name, fname),
			fmt.Sprintf("`picocalc.%s.%s(", module.Name, fname),
			fmt.Sprintf(":%s(", fname[strings.LastIndex(fname, ".")+1:]), // method syntax e.g. player:load(
		}
		found := false
		for _, pattern := range searchPatterns {
			if strings.Contains(docContent, pattern) {
				found = true
				break
			}
		}
		module.Functions[i].Documented = found
	}

	for i := range module.Constants {
		searchPatterns := []string{
			fmt.Sprintf("picocalc.%s.%s", module.Name, module.Constants[i].Name),
			fmt.Sprintf("`picocalc.%s.%s`", module.Name, module.Constants[i].Name),
			fmt.Sprintf("**%s**", module.Constants[i].Name),
		}
		found := false
		for _, pattern := range searchPatterns {
			if strings.Contains(docContent, pattern) {
				found = true
				break
			}
		}
		module.Constants[i].Documented = found
	}
}

func output(modules []Module) {
	var out *os.File
	var err error

	if outputPath != "" {
		out, err = os.Create(outputPath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error creating output file: %v\n", err)
			os.Exit(1)
		}
		defer out.Close()
	} else {
		out = os.Stdout
	}

	fmt.Fprintln(out, "# PicoDeck Lua SDK Manifest")
	fmt.Fprintln(out, "")
	fmt.Fprintln(out, "Auto-generated by `scripts/list_lua_api/main.go`")
	fmt.Fprintln(out, "")

	totalFuncs := 0
	totalDocFuncs := 0
	totalConsts := 0
	totalDocConsts := 0

	for _, module := range modules {
		funcs := module.Functions
		consts := module.Constants

		if undocumentedOnly {
			filteredFuncs := []Function{}
			for _, f := range funcs {
				if !f.Documented {
					filteredFuncs = append(filteredFuncs, f)
				}
			}
			funcs = filteredFuncs

			filteredConsts := []Constant{}
			for _, c := range consts {
				if !c.Documented {
					filteredConsts = append(filteredConsts, c)
				}
			}
			consts = filteredConsts
		}

		if len(funcs) == 0 && len(consts) == 0 {
			continue
		}

		fmt.Fprintf(out, "## picocalc.%s\n", module.Name)
		fmt.Fprintln(out, "")

		if len(funcs) > 0 {
			fmt.Fprintln(out, "### Functions")
			fmt.Fprintln(out, "")
			fmt.Fprintln(out, "| Function | Status |")
			fmt.Fprintln(out, "|----------|--------|")
			sort.Slice(funcs, func(i, j int) bool {
				return funcs[i].Name < funcs[j].Name
			})
			for _, f := range funcs {
				status := "✅ documented"
				if !f.Documented {
					status = "❌ undocumented"
				}
				fmt.Fprintf(out, "| %s | %s |\n", f.Name, status)
			}
			fmt.Fprintln(out, "")
		}

		if len(consts) > 0 {
			fmt.Fprintln(out, "### Constants")
			fmt.Fprintln(out, "")
			fmt.Fprintln(out, "| Constant | Status |")
			fmt.Fprintln(out, "|----------|--------|")
			sort.Slice(consts, func(i, j int) bool {
				return consts[i].Name < consts[j].Name
			})
			for _, c := range consts {
				status := "✅ documented"
				if !c.Documented {
					status = "❌ undocumented"
				}
				fmt.Fprintf(out, "| %s | %s |\n", c.Name, status)
			}
			fmt.Fprintln(out, "")
		}

		fmt.Fprintln(out, "---")
		fmt.Fprintln(out, "")

		if !undocumentedOnly {
			docFuncs := 0
			for _, f := range module.Functions {
				if f.Documented {
					docFuncs++
				}
			}
			docConsts := 0
			for _, c := range module.Constants {
				if c.Documented {
					docConsts++
				}
			}
			totalFuncs += len(module.Functions)
			totalDocFuncs += docFuncs
			totalConsts += len(module.Constants)
			totalDocConsts += docConsts
		}
	}

	if showSummary && !undocumentedOnly {
		fmt.Fprintln(out, "## Summary")
		fmt.Fprintln(out, "")
		fmt.Fprintln(out, "| Module | Functions | Documented | Constants | Documented |")
		fmt.Fprintln(out, "|--------|-----------|------------|-----------|------------|")
		for _, module := range modules {
			docFuncs := 0
			for _, f := range module.Functions {
				if f.Documented {
					docFuncs++
				}
			}
			docConsts := 0
			for _, c := range module.Constants {
				if c.Documented {
					docConsts++
				}
			}

			funcStr := fmt.Sprintf("%d (%s)", docFuncs, pct(docFuncs, len(module.Functions)))
			if len(module.Functions) == 0 {
				funcStr = "-"
			}
			constStr := fmt.Sprintf("%d", len(module.Constants))
			if len(module.Constants) == 0 {
				constStr = "-"
			}

			fmt.Fprintf(out, "| %s | %d | %s | %s | %s |\n",
				module.Name, len(module.Functions), funcStr, constStr,
				pct(docConsts, len(module.Constants)))
		}

		fmt.Fprintf(out, "| **Total** | **%d** | **%d (%s)** | **%d** | **%s** |\n",
			totalFuncs, totalDocFuncs, pct(totalDocFuncs, totalFuncs), totalConsts,
			pct(totalDocConsts, totalConsts))
		fmt.Fprintln(out, "")
	}
}

// pct formats n/total as a percentage, or "-" when there is nothing to count.
func pct(n, total int) string {
	if total == 0 {
		return "-"
	}
	return fmt.Sprintf("%d%%", n*100/total)
}
