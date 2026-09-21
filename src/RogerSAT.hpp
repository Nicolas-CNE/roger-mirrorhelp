#ifndef ROGER_SAT_HPP
#define ROGER_SAT_HPP

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <minisat/core/Solver.h>

struct PackageSpec {
    std::string name;
    std::string version;
    std::string url;
    std::string sha256;
    std::vector<std::string> depends;
    bool explicit_installed = false;
};

class DependencyResolverSAT {
private:
    std::map<std::string, PackageSpec> db;

    // Lista de protección base del sistema
    const std::set<std::string> protected_base_pkgs = {
        "glibc", "ncurses", "gcc-libs", "zlib", "bash", 
        "coreutils", "binutils", "linux-api-headers", "zstd", "tar"
    };

public:
    void addPackage(const PackageSpec& pkg) {
        db[pkg.name] = pkg;
    }

    PackageSpec getPackageSpec(const std::string& name) const {
        auto it = db.find(name);
        if (it != db.end()) {
            return it->second;
        }
        return PackageSpec();
    }

    bool isProtected(const std::string& name) const {
        return protected_base_pkgs.count(name) > 0;
    }

    bool resolveInstall(const std::string& target, std::vector<std::string>& out_install_list) {
        if (db.find(target) == db.end()) {
            std::cerr << "[ERROR] El paquete '" << target << "' no existe en el índice.\n";
            return false;
        }

        Minisat::Solver solver;
        std::map<std::string, Minisat::Var> pkg_to_var;

        for (const auto& [name, pkg] : db) {
            pkg_to_var[name] = solver.newVar();
        }

        for (const auto& [name, pkg] : db) {
            Minisat::Var p_var = pkg_to_var[name];
            for (auto dep : pkg.depends) {
                // Sanitizar dependencia
                dep.erase(0, dep.find_first_not_of(" \t"));
                dep.erase(dep.find_last_not_of(" \t\r\n") + 1);
                if (dep.empty()) continue;

                if (pkg_to_var.find(dep) == pkg_to_var.end()) {
                    std::cerr << "[ERROR] Dependencia rota: '" << dep << "' no existe en el índice.\n";
                    return false;
                }
                Minisat::Var dep_var = pkg_to_var[dep];
                Minisat::vec<Minisat::Lit> clause;
                clause.push(~Minisat::mkLit(p_var));  // ~Pkg
                clause.push(Minisat::mkLit(dep_var));  // v Dep
                solver.addClause_(clause);
            }
        }

        // Forzar la activación de la meta
        solver.addClause(Minisat::mkLit(pkg_to_var[target]));

        if (solver.solve()) {
            out_install_list.clear();
            for (const auto& [name, var] : pkg_to_var) {
                if (solver.modelValue(var) == Minisat::l_True) {
                    out_install_list.push_back(name);
                }
            }
            return true;
        }
        return false;
    }

    std::vector<std::string> findOrphans(const std::map<std::string, PackageSpec>& installed_pkgs) {
        std::vector<std::string> orphans;

        for (const auto& [pkg_name, pkg_info] : installed_pkgs) {
            // Regla 1: Ignorar si fue instalado explícitamente O si es paquete base protegido
            if (pkg_info.explicit_installed || isProtected(pkg_name)) {
                continue;
            }

            // Regla 2: Verificar si algún paquete instalado lo necesita
            bool is_needed = false;
            for (const auto& [other_name, other_info] : installed_pkgs) {
                if (other_name == pkg_name) continue;

                auto it = std::find(other_info.depends.begin(), other_info.depends.end(), pkg_name);
                if (it != other_info.depends.end()) {
                    is_needed = true;
                    break;
                }
            }

            if (!is_needed) {
                orphans.push_back(pkg_name);
            }
        }
        return orphans;
    }
};

#endif
