#!/bin/bash
for file in output_folder/shape_*_info.txt; do
    id=$(grep "Shape ID:" "$file" | awk '{print $3}')
    ver=$(grep "Version:" "$file" | awk '{print $2}')
    ./shape_to_svg "output_folder/shape_${id}.dat" "$ver" "output_folder/shape_${id}.svg"
done
