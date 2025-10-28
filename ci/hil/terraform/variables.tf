variable "project" {
  description = "Project slug used for tagging resources"
  type        = string
}

variable "environment" {
  description = "Deployment environment label"
  type        = string
  default     = "ci"
}

variable "region" {
  description = "AWS region for the runner"
  type        = string
  default     = "us-east-1"
}

variable "vpc_id" {
  description = "VPC identifier where the runner will be deployed"
  type        = string
}

variable "subnet_id" {
  description = "Subnet identifier for the runner"
  type        = string
}

variable "ami_id" {
  description = "Base AMI with AVX-512 capable kernel"
  type        = string
}

variable "instance_type" {
  description = "Instance type that exposes AVX-512 instructions"
  type        = string
  default     = "c6i.8xlarge"
}

variable "associate_public_ip" {
  description = "Associate a public IPv4 address with the runner"
  type        = bool
  default     = true
}

variable "ssh_key_name" {
  description = "SSH key pair used for provisioning"
  type        = string
}

variable "ssh_private_key_path" {
  description = "Local path to the SSH private key"
  type        = string
  default     = "~/.ssh/id_rsa"
}

variable "runner_registration_token" {
  description = "CI coordinator registration token"
  type        = string
  sensitive   = true
}

variable "runner_coordinator_url" {
  description = "Base URL for the CI coordinator (e.g. https://gitlab.example.com)"
  type        = string
}

variable "runner_tags" {
  description = "Tags advertised by the runner"
  type        = list(string)
  default     = ["hil", "avx512"]
}

variable "allowed_ssh_cidrs" {
  description = "List of CIDR ranges allowed to SSH into the runner"
  type        = list(string)
  default     = ["0.0.0.0/0"]
}

variable "tags" {
  description = "Additional tags to attach to resources"
  type        = map(string)
  default     = {}
}

variable "core_count" {
  description = "Number of physical cores to expose"
  type        = number
  default     = 8
}

variable "root_volume_size" {
  description = "Size of the root volume in GiB"
  type        = number
  default     = 100
}
