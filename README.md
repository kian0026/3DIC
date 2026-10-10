# eda-elite-challenge2026

## Create image:

docker build -t ubuntu2204-python3.10:1.0.0 -f Dockerfile-python3 ./

## Start container:

docker run -it --name score -e LD\_LIBRARY\_PATH=$LD\_LIBRARY\_PATH:/app/eda-elite-challenge2026/evaluator -e PYTHONPATH=/app/eda-elite-challenge2026/evaluator ubuntu2204-python3.10:1.0.0 /bin/bash



