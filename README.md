# NexusProxy

NexusProxy is a C++ project for learning how to package and run a small server application with Docker and Kubernetes.

The project currently includes:

- A simple C++ program that prints a greeting
- A Dockerfile that builds and runs the program in a Linux container
- A Kubernetes Deployment manifest for running the container with Minikube

## Run with Docker

```bash
docker build -t nexusproxy:1.0.0 .
docker run --rm nexusproxy:1.0.0
