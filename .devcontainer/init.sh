#!/bin/bash
cd "$(dirname "$0")/.."

echo "Setting up the development environment..."
echo "* Current directory: $(pwd)"

if [ -e .west ]; then
    echo "* West is already initialized."
else
    echo "* Initializing West..."
    west init -l west --mf west-test-isolated.yml
    west update --narrow
    west zephyr-export
fi

# Install pre-commit if not already installed
if ! command -v pre-commit &> /dev/null; then
    echo "* Installing pre-commit..."
    pip3 install pre-commit --break-system-packages
fi

pre-commit install
