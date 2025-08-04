if [ $# -ne 2 ]; then
  echo "expected 2 args"
  exit 1
fi
if [ ! -d "$1" ]; then
  echo "expected directory"
  exit 1
fi
num_files=$(find "$1" -type f -name "*" | wc -l)
num_lines=$(grep -r "$2" "$1" | wc -l)
echo "The number of files are ${num_files} and the number of matching lines are ${num_lines}"
