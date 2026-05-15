{
  description = "LocalVQE — CPU inference build environment (cmake + gcc + libsndfile)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in
    {
      devShells.${system} = {
        default = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.gcc
            pkgs.pkg-config
            pkgs.libsndfile
            # Optional: used when building with -DLOCALVQE_VULKAN=ON
            pkgs.vulkan-loader
            pkgs.vulkan-headers
            pkgs.shaderc
          ];
        };

        # Fuzzing shell: clang with libFuzzer + ASan/UBSan. Use with
        #   nix develop .#fuzz
        # then configure with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        # -DLOCALVQE_FUZZ=ON. See ggml/fuzz/README.md for the full recipe.
        fuzz = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.clang
            pkgs.llvm
            pkgs.pkg-config
            pkgs.libsndfile
          ];
        };

        # OBS plugin shell: libobs headers + CMake config alongside the
        # parent project's build deps (the plugin links libvqe.so, so you
        # typically build both in the same tree). Use with
        #   nix develop .#obs-plugin
        # then configure obs-plugin/ as a normal CMake project.
        obs-plugin = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.gcc
            pkgs.pkg-config
            pkgs.libsndfile
            pkgs.obs-studio
          ];
        };
      };
    };
}
