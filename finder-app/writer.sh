if [ $# -ne 2 ]; then
  echo "incorrect number of args"
  exit 1
fi
mkdir -p $(dirname "$1")
if [ $? -ne 0 ]; then
  echo "failed writing to file"
  exit 1
fi
echo "$2" > "$1"
if [ $? -ne 0 ]; then
  echo "failed writing to file"
  exit 1
fi
