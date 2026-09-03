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
  description = "Debian/Ubuntu-compatible base AMI exposing AVX-512"
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
  default     = false
}

variable "ssh_key_name" {
  description = "SSH key pair used for provisioning"
  type        = string
}

variable "runner_authentication_token_parameter_arn" {
  description = "ARN of an existing SSM SecureString containing the GitLab glrt- runner authentication token"
  type        = string

  validation {
    condition     = can(regex("^arn:[^:]+:ssm:[^:]+:[0-9]{12}:parameter/.+", var.runner_authentication_token_parameter_arn))
    error_message = "Provide a complete SSM parameter ARN."
  }
}

variable "runner_coordinator_url" {
  description = "Base URL for the CI coordinator (e.g. https://gitlab.example.com)"
  type        = string
}

variable "allowed_ssh_cidrs" {
  description = "Explicit trusted CIDR ranges allowed to SSH into the runner"
  type        = list(string)
  validation {
    condition = length(var.allowed_ssh_cidrs) > 0 && alltrue([
      for cidr in var.allowed_ssh_cidrs : cidr != "0.0.0.0/0" && cidr != "::/0"
    ])
    error_message = "Provide at least one trusted SSH CIDR; world-open CIDRs are not accepted."
  }
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
