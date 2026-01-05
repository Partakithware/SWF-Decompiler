You are soley responsible for your use and any potential damages caused from your use of anything in this repo "SWF-Decompiler", they come as is. 

# SWF-Decompiler
This suite consists of three C++ tools designed to reverse-engineer Flash (SWF) files. (Fair use) 

Unlike standard decompilers, this suite focuses on raw extraction and reconstruction of ActionScript 3 (AVM2) bytecode and vector shape data.

2. Compilation

You will need zlib installed to handle compressed SWF files (CWS).

# Install dependencies
sudo apt-get install zlib1g-dev

# Compile the extractor
g++ -o swf_extract swf_extractor.cpp -lz

# Compile the ABC decompiler
g++ -std=c++20 -o abcdec_s2 abcdec_s2.cpp

# Compile the Shape-to-SVG converter
g++ -o shape_to_svg shape_to_svg.cpp

3. Usage Guide
Stage 1: The Extraction

The swf_extract tool peels apart the SWF tags, decompresses the data, and saves images and ABC bytecode blocks into an output folder.

./swf_extract input.swf output_folder/

Stage 2: Bytecode Decompilation

Once you have the .abc files from Stage 1, use abcdec_s2 to reconstruct the .as class files. This tool maps the Constant Pool and simulates the stack to produce readable ActionScript.

./abcdec_s2 output_folder/abc_0.abc

The resulting .as files will be organized into their original package structures (com/, org/, net/, etc.).

Stage 3: Vector Reconstruction

Binary shape data extracted in Stage 1 can be converted to modern SVG format.

./shape_to_svg output_folder/shape_1.dat 4 output_folder/shape_1.svg

(A .sh was provided to batch this process)

4. Technical Notes & Troubleshooting

    Alignment: The ABC parser uses a custom readU30 implementation. If you encounter nonsensical numbers, verify the byte alignment at the start of the DoABC tag.

    Stack Guards: The decompiler currently utilizes a stack-based reconstruction. If the stack underflows, it may default to 0. or this. prefixes for property lookups.

    Coordinate Space: All vector coordinates are processed in Twips (1/20th of a pixel) per the SWF specification.

This is an early edition and does not perfectly present you with a decompiled SWF. 
If you are attempting to learn or extend from a small base project into something more this might be a nice place to start.



